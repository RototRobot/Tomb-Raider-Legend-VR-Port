#include "../dxvk/dxvk_include.h"

#include "d3d9_vr.h"

#include "d3d9_include.h"
#include "d3d9_surface.h"
#include "d3d9_device.h"

namespace dxvk {

  class D3D9VR final : public ComObjectClamp<IDirect3DVR9> {

  public:

    D3D9VR(IDirect3DDevice9* pDevice)
      : m_device(static_cast<D3D9DeviceEx*>(pDevice)) {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject) {
      if (ppvObject == nullptr)
        return E_POINTER;

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDirect3DVR9)) {
        *ppvObject = ref(this);
        return S_OK;
      }

      Logger::warn("D3D9VR::QueryInterface: Unknown interface query");
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetVRDesc(
            IDirect3DSurface9*    pSurface,
            D3D9_TEXTURE_VR_DESC* pDesc) {
      if (unlikely(pSurface == nullptr || pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9Surface* surface = static_cast<D3D9Surface*>(pSurface);
      const auto* tex = surface->GetCommonTexture();

      if (unlikely(tex == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      const auto* desc   = tex->Desc();
      const auto& device = tex->Device()->GetDXVKDevice();

      // Pin the image before its handle leaves DXVK (lesson from SiN VR
      // 1.0.2, 2026-10-04): DXVK 3.x relocates images when video memory is
      // tight or fragmented and frees the old copy once *its own* work is
      // done -- it cannot know the VR compositor is still copying from the
      // handle, which crashed inside the NVIDIA driver (null + 0x1E0).
      // stableGpuAddress takes the image out of relocation for good, as
      // D3D11Device::LockImage does for shared images. Once per image.
      if (image->canRelocate()) {
        bool pinned = false;
        auto chunk = m_device->AllocCsChunk();
        chunk->push([
          cImage = image,
          &pinned
        ] (DxvkContext* ctx) {
          DxvkImageUsageInfo usageInfo;
          usageInfo.usage = cImage->info().usage;
          usageInfo.stableGpuAddress = VK_TRUE;
          pinned = ctx->ensureImageCompatibility(cImage, usageInfo);
        });
        m_device->InjectCsChunk(std::move(chunk), true);

        if (pinned && !image->canRelocate()) {
          Logger::info(str::format("D3D9VR: pinned ", desc->Width, "x",
            desc->Height, " image against relocation"));
        } else {
          Logger::warn(str::format("D3D9VR: ", desc->Width, "x",
            desc->Height, " image STILL RELOCATABLE"));
        }
      }

      pDesc->Image            = uint64_t(image->handle());
      pDesc->Device           = device->handle();
      pDesc->PhysicalDevice   = device->adapter()->handle();
      pDesc->Instance         = device->instance()->handle();
      pDesc->Queue            = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueFamily;

      pDesc->Width            = desc->Width;
      pDesc->Height           = desc->Height;

      // The linear format, not FormatSrgb. OpenVR is told the colour space
      // separately through Texture_t::eColorSpace, so handing it the sRGB view
      // format here would double-correct.
      pDesc->Format           = tex->GetFormatMapping().Format;
      pDesc->SampleCount      = uint32_t(image->info().sampleCount);

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE TransferSurface(
            IDirect3DSurface9* pSurface,
            BOOL               waitResourceIdle) {
      if (unlikely(pSurface == nullptr))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDevice();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      if (unlikely(tex == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& image = tex->GetImage();
      if (unlikely(image == nullptr))
        return D3DERR_INVALIDCALL;

      VkImageSubresourceRange subresources = {
        VK_IMAGE_ASPECT_COLOR_BIT,
        0, image->info().mipLevels,
        0, image->info().numLayers
      };

      m_device->TransformImage(
        tex, &subresources,
        image->info().layout,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

      if (waitResourceIdle) {
        m_device->WaitForResource(*image,
          tex->GetMappingBufferSequenceNumber(0u), D3DLOCK_READONLY);
      }

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockDevice() {
      m_lock = m_device->LockDevice();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockDevice() {
      m_lock = D3D9DeviceLock();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE LockSubmissionQueue() {
      // Drain DXVK's CPU-side command stream while no other D3D9 call can add
      // to it, then take the queue's external-synchronisation gate. Once this
      // returns the device lock may be dropped: the game can record the next
      // frame, but DXVK's submit thread cannot touch VkQueue until the eye
      // textures have gone to the compositor.
      D3D9DeviceLock lock = m_device->LockDevice();
      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->lockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() {
      m_device->GetDXVKDevice()->unlockSubmission();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE WaitDeviceIdle() {
      // Called from the present path while the game's own threads are live.
      // The D3D9 command chunk is device-owned mutable state, so flushing
      // without the same lock the draw calls take would race them.
      D3D9DeviceLock lock = m_device->LockDevice();

      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceDesc(
            D3D9_VR_DEVICE_DESC* pDesc) {
      if (unlikely(pDesc == nullptr))
        return D3DERR_INVALIDCALL;

      const auto& device = m_device->GetDXVKDevice();

      pDesc->Instance         = device->instance()->handle();
      pDesc->PhysicalDevice   = device->adapter()->handle();
      pDesc->Device           = device->handle();
      pDesc->Queue            = device->queues().graphics.queueHandle;
      pDesc->QueueFamilyIndex = device->queues().graphics.queueFamily;
      pDesc->QueueIndex       = device->queues().graphics.queueIndex;
      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE CopySurfaceToImage(
            IDirect3DSurface9*            pSurface,
      const D3D9_VR_EXTERNAL_IMAGE*       pDst,
            INT                           DstX,
            INT                           DstY,
            BOOL                          WaitForCopy) {
      if (unlikely(pSurface == nullptr || pDst == nullptr || !pDst->Image))
        return D3DERR_INVALIDCALL;

      D3D9DeviceLock lock = m_device->LockDevice();

      auto* tex = static_cast<D3D9Surface*>(pSurface)->GetCommonTexture();
      if (unlikely(tex == nullptr))
        return D3DERR_INVALIDCALL;

      const Rc<DxvkImage> srcImage = tex->GetImage();
      if (unlikely(srcImage == nullptr
       || srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT))
        return D3DERR_INVALIDCALL;

      const VkExtent3D extent = srcImage->mipLevelExtent(0);

      if (unlikely(DstX < 0 || DstY < 0
       || uint32_t(DstX) + extent.width  > pDst->Width
       || uint32_t(DstY) + extent.height > pDst->Height))
        return D3DERR_INVALIDCALL;

      Rc<DxvkImage> dstImage = WrapExternalImage(*pDst);
      if (unlikely(dstImage == nullptr))
        return D3DERR_INVALIDCALL;

      const VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
      const VkFormat srcFormat = srcImage->info().format;

      if (LinearFormat(srcFormat) == LinearFormat(pDst->Format)) {
        // Same channel layout: the bits go across unchanged, which is what
        // is wanted even from UNORM to SRGB -- the game's back buffer holds
        // gamma-encoded colour in a UNORM image.
        m_device->EmitCs([
          cDstImage  = dstImage,
          cSrcImage  = srcImage,
          cLayers    = layers,
          cDstOffset = VkOffset3D { DstX, DstY, 0 },
          cExtent    = extent
        ] (DxvkContext* ctx) {
          ctx->copyImage(
            cDstImage, cLayers, cDstOffset,
            cSrcImage, cLayers, VkOffset3D { 0, 0, 0 },
            cExtent);
        });
      } else {
        // Different channel order (BGRA into RGBA): a blit. Reading the
        // source through its sRGB view when the destination is sRGB makes
        // the decode and the encode cancel, so the colours still match.
        if (!m_blitReported) {
          m_blitReported = true;
          Logger::warn(str::format("D3D9VR: source format ", srcFormat,
            " and swapchain format ", pDst->Format,
            " differ in layout; blitting instead of copying"));
        }

        const VkFormat srcSrgb = tex->GetFormatMapping().FormatSrgb;

        DxvkImageViewKey dstViewInfo;
        dstViewInfo.viewType   = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        dstViewInfo.usage      = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        dstViewInfo.format     = pDst->Format;
        dstViewInfo.aspects    = VK_IMAGE_ASPECT_COLOR_BIT;
        dstViewInfo.mipIndex   = 0;
        dstViewInfo.mipCount   = 1;
        dstViewInfo.layerIndex = 0;
        dstViewInfo.layerCount = 1;

        DxvkImageViewKey srcViewInfo = dstViewInfo;
        srcViewInfo.viewType   = VK_IMAGE_VIEW_TYPE_2D;
        srcViewInfo.usage      = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        srcViewInfo.format     = (IsSrgbFormat(pDst->Format) && srcSrgb)
          ? srcSrgb : srcFormat;

        std::array<VkOffset3D, 2> dstOffsets = {{
          { DstX, DstY, 0 },
          { DstX + int32_t(extent.width), DstY + int32_t(extent.height), 1 } }};
        std::array<VkOffset3D, 2> srcOffsets = {{
          { 0, 0, 0 },
          { int32_t(extent.width), int32_t(extent.height), 1 } }};

        m_device->EmitCs([
          cDstView    = dstImage->createView(dstViewInfo),
          cSrcView    = srcImage->createView(srcViewInfo),
          cDstOffsets = dstOffsets,
          cSrcOffsets = srcOffsets
        ] (DxvkContext* ctx) {
          ctx->blitImageView(
            cDstView, cDstOffsets.data(),
            cSrcView, cSrcOffsets.data(),
            VK_FILTER_NEAREST);
        });
      }

      if (WaitForCopy) {
        m_device->WaitForResource(*dstImage,
          DxvkCsThread::SynchronizeAll, 0);
      }

      return D3D_OK;
    }

    HRESULT STDMETHODCALLTYPE ReleaseExternalImages() {
      if (m_externalImages.empty())
        return D3D_OK;

      // Nothing may still be reading or writing them when the runtime
      // destroys the swapchain behind these handles.
      D3D9DeviceLock lock = m_device->LockDevice();

      m_device->Flush();
      m_device->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
      m_device->GetDXVKDevice()->waitForIdle();
      m_externalImages.clear();
      return D3D_OK;
    }

  private:

    static VkFormat LinearFormat(VkFormat Format) {
      switch (Format) {
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        default:                      return Format;
      }
    }

    static bool IsSrgbFormat(VkFormat Format) {
      return LinearFormat(Format) != Format;
    }

    // DXVK only copies between images it knows, so each swapchain image is
    // imported once. shared makes DXVK return it to its default layout at
    // the end of every submission, which is the layout the runtime expects
    // it released in.
    Rc<DxvkImage> WrapExternalImage(const D3D9_VR_EXTERNAL_IMAGE& Image) {
      for (const auto& entry : m_externalImages) {
        if (entry.first == Image.Image)
          return entry.second;
      }

      DxvkImageCreateInfo info;
      info.type          = VK_IMAGE_TYPE_2D;
      info.format        = Image.Format;
      info.flags         = 0u;
      info.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      info.extent        = { Image.Width, Image.Height, 1u };
      info.numLayers     = 1u;
      info.mipLevels     = 1u;
      info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                         | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                         | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                         | VK_IMAGE_USAGE_SAMPLED_BIT;
      info.stages        = VK_PIPELINE_STAGE_TRANSFER_BIT
                         | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                         | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      info.access        = VK_ACCESS_TRANSFER_WRITE_BIT
                         | VK_ACCESS_TRANSFER_READ_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                         | VK_ACCESS_SHADER_READ_BIT;
      info.tiling        = VK_IMAGE_TILING_OPTIMAL;
      info.layout        = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      info.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      info.shared        = VK_TRUE;
      info.debugName     = "OpenXR swapchain image";

      Rc<DxvkImage> image = m_device->GetDXVKDevice()->importImage(
        info, VkImage(Image.Image), VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      m_externalImages.emplace_back(Image.Image, image);
      Logger::info(str::format("D3D9VR: imported ", Image.Width, "x",
        Image.Height, " swapchain image, format ", Image.Format));
      return image;
    }

    D3D9DeviceEx*  m_device;
    D3D9DeviceLock m_lock;

    std::vector<std::pair<uint64_t, Rc<DxvkImage>>> m_externalImages;
    bool m_blitReported = false;

  };

}

extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface) {
  if (pDevice == nullptr || ppInterface == nullptr)
    return D3DERR_INVALIDCALL;

  // DXVK's COM objects start with no references; hand this one out owned,
  // so the caller's QueryInterface / Release pair cannot destroy it.
  *ppInterface = dxvk::ref(new dxvk::D3D9VR(pDevice));
  return D3D_OK;
}

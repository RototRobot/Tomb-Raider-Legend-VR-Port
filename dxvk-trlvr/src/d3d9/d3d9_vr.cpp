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
      pDesc->QueueFamilyIndex = device->queues().graphics.queueIndex;

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

  private:

    D3D9DeviceEx*  m_device;
    D3D9DeviceLock m_lock;

  };

}

extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface) {
  if (pDevice == nullptr || ppInterface == nullptr)
    return D3DERR_INVALIDCALL;

  *ppInterface = new dxvk::D3D9VR(pDevice);
  return D3D_OK;
}

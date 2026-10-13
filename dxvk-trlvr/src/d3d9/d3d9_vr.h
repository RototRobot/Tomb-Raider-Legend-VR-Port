#pragma once

// IDirect3DVR9 -- hands the Vulkan handles behind a D3D9 surface to a VR runtime.
//
// OpenVR cannot be given a Direct3D 9 texture; its Submit takes D3D11, a DXGI
// shared handle, OpenGL, D3D12 or Vulkan, and nothing else. But DXVK *is*
// Vulkan underneath, so the image the game just rendered is already a VkImage.
// This interface exposes it, which makes submission free: no readback, no
// staging copy, and no D3D9Ex shared-surface detour.
//
// D3D9_TEXTURE_VR_DESC deliberately mirrors the leading members of
// vr::VRVulkanTextureData_t, so a filled desc can be memcpy'd or cast straight
// into a Submit call.
//
// The mod now runs on OpenXR, whose swapchain images belong to the runtime:
// the session is created from GetDeviceDesc and each frame is copied in with
// CopySurfaceToImage. GetVRDesc and TransferSurface are the OpenVR-era
// hand-over and are kept for a runtime that takes the image in place.
//
// The design follows the d3d9_vr interface in the L4D2VR and SiN VR DXVK forks.
// This is a fresh implementation against DXVK 3.1, whose device-locking API
// differs from the 3.0.2 those were written for.

#include <d3d9.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#undef VK_USE_PLATFORM_WIN32_KHR

struct D3D9_TEXTURE_VR_DESC {
  uint64_t         Image;            // VkImage; OpenVR types it as uint64_t
  VkDevice         Device;
  VkPhysicalDevice PhysicalDevice;
  VkInstance       Instance;
  VkQueue          Queue;
  uint32_t         QueueFamilyIndex;

  uint32_t         Width;
  uint32_t         Height;
  VkFormat         Format;
  uint32_t         SampleCount;
};

// What an OpenXR session is created from (XrGraphicsBindingVulkanKHR).
struct D3D9_VR_DEVICE_DESC {
  VkInstance       Instance;
  VkPhysicalDevice PhysicalDevice;
  VkDevice         Device;
  VkQueue          Queue;
  uint32_t         QueueFamilyIndex;
  uint32_t         QueueIndex;       // within the family
};

// A VkImage DXVK does not own: an OpenXR swapchain image. The runtime hands
// these out in COLOR_ATTACHMENT_OPTIMAL and wants them back that way.
struct D3D9_VR_EXTERNAL_IMAGE {
  uint64_t         Image;
  VkFormat         Format;
  uint32_t         Width;
  uint32_t         Height;
};

MIDL_INTERFACE("b1e5c0a4-3f7d-4a92-8c16-2d9e47f0a3b5")
IDirect3DVR9 : public IUnknown {
  // Fills pDesc with the Vulkan handles backing pSurface.
  virtual HRESULT STDMETHODCALLTYPE GetVRDesc(
          IDirect3DSurface9*    pSurface,
          D3D9_TEXTURE_VR_DESC* pDesc) = 0;

  // Moves the surface into TRANSFER_SRC_OPTIMAL, the layout the compositor
  // requires on submit. Pass TRUE to also wait for the resource to be idle.
  virtual HRESULT STDMETHODCALLTYPE TransferSurface(
          IDirect3DSurface9* pSurface,
          BOOL               waitResourceIdle) = 0;

  virtual HRESULT STDMETHODCALLTYPE LockDevice() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockDevice() = 0;

  // VkQueue is externally synchronised and DXVK's own submit thread uses it,
  // so it has to be held across an OpenVR submit or the two race.
  virtual HRESULT STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockSubmissionQueue() = 0;

  virtual HRESULT STDMETHODCALLTYPE WaitDeviceIdle() = 0;

  // --- OpenXR ---------------------------------------------------------------
  // OpenXR swapchain images belong to the runtime, so a frame is copied into
  // one rather than submitted in place. The copy is recorded through DXVK's
  // own context, which keeps its layout and lifetime tracking correct and
  // puts the copy on the queue in order with the frame it reads.

  virtual HRESULT STDMETHODCALLTYPE GetDeviceDesc(
          D3D9_VR_DEVICE_DESC* pDesc) = 0;

  // Copies all of pSurface to (DstX, DstY) in the external image. With
  // WaitForCopy the call returns once the GPU has finished it. Must not be
  // called with the submission queue locked.
  virtual HRESULT STDMETHODCALLTYPE CopySurfaceToImage(
          IDirect3DSurface9*            pSurface,
    const D3D9_VR_EXTERNAL_IMAGE*       pDst,
          INT                           DstX,
          INT                           DstY,
          BOOL                          WaitForCopy) = 0;

  // Drops every wrapper made for an external image, after the GPU is done
  // with them. Call before the runtime destroys the swapchain.
  virtual HRESULT STDMETHODCALLTYPE ReleaseExternalImages() = 0;
};

// NOTE: the locks above only lock anything if the device was created with
// D3DCREATE_MULTITHREADED. Without it DXVK's AcquireLock hands back an empty
// guard. Tomb Raider: Legend does not ask for it (its flags are
// HARDWARE_VERTEXPROCESSING | PUREDEVICE), so the proxy forces the flag on at
// CreateDevice rather than this fork weakening DXVK's locking.

#ifdef _MSC_VER
struct __declspec(uuid("b1e5c0a4-3f7d-4a92-8c16-2d9e47f0a3b5")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0xb1e5c0a4, 0x3f7d, 0x4a92, 0x8c, 0x16, 0x2d, 0x9e, 0x47, 0xf0, 0xa3, 0xb5);
#endif

// Exported from d3d9.dll. Resolve with GetProcAddress and pass the live device.
extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface);

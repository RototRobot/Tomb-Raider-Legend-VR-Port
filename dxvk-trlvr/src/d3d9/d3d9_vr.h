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

MIDL_INTERFACE("7d4a1e93-2c68-4b51-9a0f-6e83c5d27a14")
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
};

// NOTE: the locks above only lock anything if the device was created with
// D3DCREATE_MULTITHREADED. Without it DXVK's AcquireLock hands back an empty
// guard. Tomb Raider: Legend does not ask for it (its flags are
// HARDWARE_VERTEXPROCESSING | PUREDEVICE), so the proxy forces the flag on at
// CreateDevice rather than this fork weakening DXVK's locking.

#ifdef _MSC_VER
struct __declspec(uuid("7d4a1e93-2c68-4b51-9a0f-6e83c5d27a14")) IDirect3DVR9;
#else
__CRT_UUID_DECL(IDirect3DVR9, 0x7d4a1e93, 0x2c68, 0x4b51, 0x9a, 0x0f, 0x6e, 0x83, 0xc5, 0xd2, 0x7a, 0x14);
#endif

// Exported from d3d9.dll. Resolve with GetProcAddress and pass the live device.
extern "C" HRESULT __stdcall Direct3DCreateVR9(
        IDirect3DDevice9* pDevice,
        IDirect3DVR9**    ppInterface);

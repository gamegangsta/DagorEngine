#include "device.h"
#include "pipeline_barrier.h"
#include "perfMon/dag_statDrv.h"
#include "device_context.h"

#if _TARGET_ANDROID
#include <osApiWrappers/dag_progGlobals.h>
#include <supp/dag_android_native_app_glue.h>
#include <sys/system_properties.h>
#include <swappy/swappyVk.h>
#endif

using namespace drv3d_vulkan;

namespace
{
const char *swap_mode_to_string(VkPresentModeKHR mode)
{
  switch (mode)
  {
    default: return "";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "VK_PRESENT_MODE_IMMEDIATE_KHR";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "VK_PRESENT_MODE_MAILBOX_KHR";
    case VK_PRESENT_MODE_FIFO_KHR: return "VK_PRESENT_MODE_FIFO_KHR";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      return "VK_PRESENT_MODE_FIFO_RELAXED_KHR";
      // we probably never use those, those are for direct front buffer rendering
#if VK_KHR_shared_presentable_image
    case VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR: return "VK_PRESENT_MODE_SHARED_DEMAND_REFRESH_KHR";
    case VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR: return "VK_PRESENT_MODE_SHARED_CONTINUOUS_REFRESH_KHR";
#endif
  }
}
// This lists the alternative modes for a requested mode, the earlier entries are
// closer match to the requested mode than the later ones.
const VkPresentModeKHR bestPresentModeMatch[4][3] = //
  {
    // VK_PRESENT_MODE_IMMEDIATE_KHR
    {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR},
    // row for VK_PRESENT_MODE_MAILBOX_KHR
    {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR},
    // row for VK_PRESENT_MODE_FIFO_KHR
    {VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR},
    // row for VK_PRESENT_MODE_FIFO_RELAXED_KHR
    {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR}};
} // namespace

Swapchain::Swapchain(Device &dvc) : device(dvc), vkDev(dvc.getVkDevice()) {}

void SwapchainMode::setPresentModeFromConfig()
{
  const DataBlock *videoCfg = ::dgs_get_settings()->getBlockByNameEx("video");
  bool useVSync = videoCfg->getBool("vsync", false);
  bool useAdaptiveVSync = videoCfg->getBool("adaptive_vsync", false);
  presentMode = get_requested_present_mode(useVSync, useAdaptiveVSync);
}

void Swapchain::destroySwapchainHandle(VulkanSwapchainKHRHandle &sc_handle)
{
  if (!is_null(sc_handle))
  {
#if ENABLE_SWAPPY
    if (swappyInitialized)
    {
      debug("vulkan: notifying Swappy about destoying swapchain");
      SwappyVk_destroySwapchain(device.getDevice(), sc_handle);
      swappyInitialized = false;
    }
#endif
    VULKAN_LOG_CALL(vkDev.vkDestroySwapchainKHR(vkDev.get(), sc_handle, nullptr));
    sc_handle = VulkanNullHandle();
  }
}

VkSurfaceCapabilitiesKHR Swapchain::querySurfaceCaps()
{
  VkSurfaceCapabilitiesKHR sc = {};
  VULKAN_LOG_CALL(
    vkDev.getInstance().vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.getDeviceProperties().device, activeMode.surface, &sc));
  return sc;
}

Swapchain::PresentModeStore Swapchain::queryPresentModeList()
{
  PresentModeStore result;
  uint32_t count = 0;
  if (VULKAN_CHECK_OK(vkDev.getInstance().vkGetPhysicalDeviceSurfacePresentModesKHR(device.getDeviceProperties().device,
        activeMode.surface, &count, nullptr)))
  {
    result.resize(count);
    if (VULKAN_CHECK_OK(vkDev.getInstance().vkGetPhysicalDeviceSurfacePresentModesKHR(device.getDeviceProperties().device,
          activeMode.surface, &count, result.data())))
    {
      result.resize(count);
    }
    else
    {
      result.clear();
    }
  }

  return result;
}

Swapchain::SwapchainFormatStore Swapchain::queryPresentFormats()
{
  SwapchainFormatStore result;

#if _TARGET_ANDROID
  if (usePredefinedPresentFormat)
  {
    // RGBA8 UNORM have 100% coverage, use it instead on asking available ones
    result.push_back(FormatStore::fromVkFormat(VK_FORMAT_R8G8B8A8_UNORM));
    return result;
  }
#endif

  uint32_t count = 0;
  if (is_null(activeMode.surface))
    return result;

  if (VULKAN_CHECK_OK(vkDev.getInstance().vkGetPhysicalDeviceSurfaceFormatsKHR(device.getDeviceProperties().device, activeMode.surface,
        &count, nullptr)))
  {
    SurfaceFormatStore formats;
    formats.resize(count);
    if (VULKAN_CHECK_OK(vkDev.getInstance().vkGetPhysicalDeviceSurfaceFormatsKHR(device.getDeviceProperties().device,
          activeMode.surface, &count, formats.data())))
    {
      for (auto &&format : formats)
        if (FormatStore::canBeStored(format.format))
          result.push_back(FormatStore::fromVkFormat(format.format));

      if (result.empty())
      {
        // check for undefined format, it indicates that you can use the format you like
        // most
        auto iter = eastl::find_if(begin(formats), end(formats),
          [](const VkSurfaceFormatKHR &fmt) //
          { return fmt.format == VK_FORMAT_UNDEFINED; });
        if (end(formats) != iter)
          result.push_back(FormatStore::fromCreateFlags(TEXFMT_DEFAULT));
      }
    }
  }

  return result;
}

VkPresentModeKHR Swapchain::selectPresentMode(const PresentModeStore &modes, VkPresentModeKHR requested)
{
  // shortcut for VK_PRESENT_MODE_FIFO_KHR as it is required to be supported
  if (VK_PRESENT_MODE_FIFO_KHR == requested || modes.empty())
    return VK_PRESENT_MODE_FIFO_KHR;

  if (end(modes) != eastl::find(begin(modes), end(modes), requested))
    return requested;

  G_ASSERTF(requested < array_size(bestPresentModeMatch),
    "vulkan: selectPresentMode(..., 'requested' = %u) is a mode that is not fully "
    "supported!");
  if (requested >= array_size(bestPresentModeMatch))
    return VK_PRESENT_MODE_FIFO_KHR;

  for (auto &&mode : bestPresentModeMatch[requested])
    if (end(modes) != eastl::find(begin(modes), end(modes), mode))
      return mode;

  return VK_PRESENT_MODE_FIFO_KHR;
}

FormatStore Swapchain::selectPresentFormat(const SwapchainFormatStore &formats)
{
  // most common format (0 -> argb), do a fast check
  if (end(formats) != eastl::find(begin(formats), end(formats), FormatStore{}))
    return FormatStore{};

  // select the format with the most bytes but that is not srgb nor float
  auto rateFormat = [](FormatStore f) -> int //
  { return f.getBytesPerPixelBlock() - (f.isSrgb << 5) - (f.isFloat() << 4); };

  FormatStore selected = formats.front();
  for (auto &&format : formats)
    if (rateFormat(format) > rateFormat(selected))
      selected = format;

  return selected;
}

bool Swapchain::checkVkSwapchainError(VkResult rc, const char *source_call)
{
  if (VK_ERROR_SURFACE_LOST_KHR == rc)
  {
    // only real possibility is that the window got closed first
    fatal("vulkan: %s = VK_ERROR_SURFACE_LOST_KHR, unrecoverable state", source_call);
    return false;
  }
  else if (VK_ERROR_OUT_OF_DATE_KHR == rc)
  {
    debug("vulkan: %s = VK_ERROR_OUT_OF_DATE_KHR, attempting to recreate swapchain", source_call);
    return false;
  }
  else if (VK_SUBOPTIMAL_KHR == rc)
  {
#if _TARGET_ANDROID
    // if preTransform is not used on android - system will yell that it is suboptimal!
    // in case if we intentionally skipped transform - silence it up too
    if (!preRotation || !preRotationProcessed)
      return true;
#endif

    // log differently but treat as out of date
    debug("vulkan: %s = VK_SUBOPTIMAL_KHR, attempting to recreate swapchain", source_call);
    return false;
  }
  else if (VULKAN_CHECK_FAIL(rc))
  {
    drv3d_vulkan::generateFaultReport();
    // out of memory or device failure
    fatal("vulkan: %s failed, can't continue. code: %i", source_call, rc);
    return false;
  }

  return true;
}

// acquire swap image should do what it named for, nothing more!
bool Swapchain::acquireSwapImage(FrameInfo &frame)
{
  // we can't get here in wrong state
  G_ASSERT((currentState == SWP_EARLY_ACQUIRE) || (currentState == SWP_DELAYED_ACQUIRE));

  // on fail conditions that are not fatal, the semaphore could be reused but there is no
  // real need to do the extra work as it gets collected on this frame end.
  VulkanSemaphoreHandle syncSemaphore = frame.allocSemaphore(vkDev, true);

  VkResult rc;
  {
    TIME_PROFILE(vulkan_swapchain_image_acquire);
    rc = VULKAN_CHECK_RESULT(
      vkDev.vkAcquireNextImageKHR(vkDev.get(), handle, GENERAL_GPU_TIMEOUT_NS, syncSemaphore, VulkanFenceHandle{}, &colorTargetIndex));
  }

  // avoid silently hangin up
  // if acquisitions leak or some sync problems are encountered
  if (rc == VK_TIMEOUT)
  {
    logerr("vulkan: timeout on swapchain image acquire, trying to restore swapchain");
    // try to remove current swapchain fully
    destroySwapchainHandle(handle);
    return false;
  }

  if (checkVkSwapchainError(rc, "vkAcquireNextImageKHR"))
  {
    device.getQueue(DeviceQueueType::GRAPHICS).addSubmitSemaphore(syncSemaphore, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    return true;
  }
  else
    return false;
}

bool Swapchain::needPreRotationFIFOWorkaround()
{
  return ::dgs_get_settings()
    ->getBlockByNameEx("vulkan")
    ->getBlockByNameEx("vendor")
    ->getBlockByNameEx(device.getDeviceProperties().vendorName)
    ->getBool("forceFIFOwithPreRotate", false);
}

Swapchain::ChangeResult Swapchain::changeSwapchain(FrameInfo &frame)
{
  // old swapchain will be not used anymore, regardless of results
  // so remove images right now
  for (auto &&image : swapImages)
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*image);
  swapImages.clear();

  // no surface - no swapchain
  if (is_null(activeMode.surface))
    return ChangeResult::RETRY;

  VkSwapchainCreateInfoKHR sci;
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.pNext = nullptr;
  sci.flags = 0;
  sci.surface = activeMode.surface;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.queueFamilyIndexCount = 0;
  sci.pQueueFamilyIndices = nullptr;
  sci.clipped = VK_FALSE;
  sci.oldSwapchain = handle;

  auto caps = querySurfaceCaps();

  if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR)
    sci.preTransform = VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
  else
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;

  if (device.getFeatures().test(DeviceFeaturesConfig::PRE_ROTATION))
  {
    preRotation = true;
    preRotationAngle = 0;

    if (preRotationProcessed)
    {
      sci.preTransform = caps.currentTransform;
      preRotationAngle = surfaceTransformToAngle(caps.currentTransform);
    }
  }

  // TODO: more checks needed
  if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  else
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

  sci.imageUsage &= caps.supportedUsageFlags; // TODO: more checks needed

  // this is dump, why use 0 to indicate "as many as you like (or memory can
  // support)"?
  // allocate extra PRESENT_ENGINE_LOCKED_IMAGES due to possible locks from presentation engine on currently presented images
  sci.minImageCount = max<uint32_t>(FRAME_FRAME_BACKLOG_LENGTH, caps.minImageCount);
  sci.minImageCount =
    min<uint32_t>(sci.minImageCount + PRESENT_ENGINE_LOCKED_IMAGES, (0 == caps.maxImageCount) ? UINT32_MAX : caps.maxImageCount);
  changeDelay = sci.minImageCount;

  if (caps.currentExtent.width == -1)
    sci.imageExtent.width = clamp<uint32_t>(activeMode.extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  else
    sci.imageExtent.width = caps.currentExtent.width;

  if (caps.currentExtent.height == -1)
    sci.imageExtent.height = clamp<uint32_t>(activeMode.extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  else
    sci.imageExtent.height = caps.currentExtent.height;

  // this can happen if the window was minimized, the only way to continue is
  // to run in 'headless' mode until the window is back to normal
  if (!sci.imageExtent.width || !sci.imageExtent.height)
    return ChangeResult::RETRY;

  // swap extents to match device transform
  if ((sci.preTransform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) || (sci.preTransform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))
    eastl::swap(sci.imageExtent.width, sci.imageExtent.height);

  debug("vulkan: swapchain: using %u images", sci.minImageCount);
  debug("vulkan: Querying output format list...");
  auto formatList = queryPresentFormats();
  if (formatList.empty())
  {
    logwarn("vulkan: Output format list was empty, aborting...");
    return ChangeResult::FAIL;
  }

  activeMode.colorFormat = selectPresentFormat(formatList);
  debug("vulkan: ...back buffer color format: %s", activeMode.colorFormat.getNameString());

  sci.imageFormat = activeMode.colorFormat.asVkFormat();
  // currently only this now
  sci.imageColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;

  debug("vulkan: Querying present mode list...");
  auto modes = queryPresentModeList();
  if (modes.empty())
  {
    logwarn("vulkan: Present mode list was empty, aborting...");
    return ChangeResult::FAIL;
  }
  else
  {
    for (auto &i : modes)
      debug("vulkan:    present mode %s available ", swap_mode_to_string(i));
  }

  sci.presentMode = selectPresentMode(modes, activeMode.presentMode);

  if (preRotation && preRotationAngle && needPreRotationFIFOWorkaround())
  {
    debug("vulkan: overriding present mode to FIFO as workaround for buggy pre rotation");
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  }

  debug("vulkan: ...using present mode %s", swap_mode_to_string(sci.presentMode));

  activeMode.presentMode = sci.presentMode;

  VulkanSwapchainKHRHandle oldHandle = handle;

  VkResult createResult = vkDev.vkCreateSwapchainKHR(vkDev.get(), &sci, nullptr, ptr(handle));

  // no need to keep old swapchain if new one fails
  if (!is_null(oldHandle))
    destroySwapchainHandle(oldHandle);

  if (createResult != VK_SUCCESS)
  {
    logwarn("vulkan: failed to create swapchain due to error %u[%s]", createResult, vulkan_error_string(createResult));
    // clear handle for sanity
    handle = VulkanSwapchainKHRHandle();
    return ChangeResult::FAIL;
  }

#if ENABLE_SWAPPY
  {
    JNIEnv *jniEnv = nullptr;

    android_app *app = (android_app *)win32_get_instance();
    app->activity->vm->AttachCurrentThread(&jniEnv, NULL);

    debug("vulkan: Initializing Swappy");

    uint64_t refreshDuration = 0;
    swappyInitialized = SwappyVk_initAndGetRefreshCycleDuration(jniEnv, app->activity->clazz, device.getPhysicalDevice(),
      device.getDevice(), handle, &refreshDuration);
    if (swappyInitialized)
    {
      debug("vulkan: Swappy is initialized on swapchain with refreshDuration: %llu ns", refreshDuration);
      SwappyVk_setWindow(device.getDevice(), handle, (ANativeWindow *)win32_get_main_wnd());
      SwappyVk_setAutoSwapInterval(false);
    }
    else
      debug("vulkan: Swappy failed to initialize on swapchain");
    G_ASSERT(swappyInitialized);

    setSwappyTargetFrameRate(swappyTarget);

    app->activity->vm->DetachCurrentThread();
  }
#endif

  // check if we can direct render without offscreen buffer
  const VkExtent3D &oext = offscreenBuffer->getBaseExtent();
  const VkExtent2D &sext = sci.imageExtent;
  bool sameExts = ((oext.width == sext.width) && (oext.height == sext.height));

  if (!activeMode.enableSrgb && sameExts && !preRotation)
  {
    destroyOffscreenBuffer(frame);
    colorTarget = nullptr;
  }

  if (preRotation && preRotationAngle)
  {
    debug("vulkan: swapchain: pre rotation transform from [%u,%u] to [%u,%u] at angle %u is active", oext.width, oext.height,
      sext.width, sext.height, preRotationAngle);
  }
  else if (!sameExts)
  {
    // size of surface & our front setup does not match
    // this can be an error or some transient process
    // so be aware of it on warning level
    logwarn("vulkan: swapchain: presenting with scaling blit from [%u,%u] to [%u,%u]", oext.width, oext.height, sext.width,
      sext.height);
  }

  destroyLastRenderedImage(frame);
  if (device.getFeatures().test(DeviceFeaturesConfig::KEEP_LAST_RENDERED_IMAGE))
    createLastRenderedImage(sci);

  return updateSwapchainImageList(sci) ? ChangeResult::OK : ChangeResult::FAIL;
}

bool Swapchain::updateSwapchainImageList(const VkSwapchainCreateInfoKHR &sci)
{
  ImageCreateInfo ii;
  ii.flags = 0;
  ii.type = VK_IMAGE_TYPE_2D;
  ii.mips = 1;
  ii.usage = sci.imageUsage;
  ii.size.width = sci.imageExtent.width;
  ii.size.height = sci.imageExtent.height;
  ii.size.depth = 1;
  ii.arrays = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;

  uint32_t count = 0;
  if (VULKAN_CHECK_FAIL(vkDev.vkGetSwapchainImagesKHR(vkDev.get(), handle, &count, nullptr)))
    return false;

  eastl::vector<VulkanImageHandle> imageList;
  imageList.resize(count);
  if (VULKAN_CHECK_FAIL(vkDev.vkGetSwapchainImagesKHR(vkDev.get(), handle, &count, ary(imageList.data()))))
    return false;

  // empty memory object indicates the image object is owned by another parent object (the swapchain)
  swapImages.reserve(imageList.size());
  {
    SharedGuardedObjectAutoLock resLock(get_device().resources);
    for (auto &&image : imageList)
    {
      Image::Description desc = {{ii.flags, VK_IMAGE_TYPE_2D, ii.size, 1, 1, ii.usage, VkSampleCountFlagBits::VK_SAMPLE_COUNT_1_BIT},
        Image::MEM_NOT_EVICTABLE, activeMode.colorFormat, VK_IMAGE_LAYOUT_UNDEFINED};
      Image *newImage = get_device().resources.alloc<Image>(desc, false);
      newImage->setDerivedHandle(image);
      get_device().setTexName(newImage, String(64, "swapchainImage%u", &image - &imageList[0]));
      swapImages.push_back(newImage);
    }
  }

  return true;
}

void Swapchain::doBlit(ExecutionContext &ctx, Image *from, Image *to)
{
  makeReadyForBlit(ctx, from, to);

  VkImageBlit blit;
  blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.srcSubresource.mipLevel = 0;
  blit.srcSubresource.baseArrayLayer = 0;
  blit.srcSubresource.layerCount = 1;
  blit.srcOffsets[0].x = 0;
  blit.srcOffsets[0].y = 0;
  blit.srcOffsets[0].z = 0;
  const auto &srcExtent = from->getBaseExtent();
  blit.srcOffsets[1].x = srcExtent.width;
  blit.srcOffsets[1].y = srcExtent.height;
  blit.srcOffsets[1].z = 1;
  blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.dstSubresource.mipLevel = 0;
  blit.dstSubresource.baseArrayLayer = 0;
  blit.dstSubresource.layerCount = 1;
  blit.dstOffsets[0].x = 0;
  blit.dstOffsets[0].y = 0;
  blit.dstOffsets[0].z = 0;
  const auto &dstExtent = to->getBaseExtent();
  blit.dstOffsets[1].x = dstExtent.width;
  blit.dstOffsets[1].y = dstExtent.height;
  blit.dstOffsets[1].z = 1;

  VULKAN_LOG_CALL(vkDev.vkCmdBlitImage(ctx.frameCore, from->getHandle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->getHandle(),
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR));
}

void Swapchain::makeReadyForBlit(ExecutionContext &ctx, Image *from, Image *to)
{
  ctx.back.syncTrack.addImageAccess(
    // present barrier is very special, as vkQueuePresent does all visibility ops
    // ref: vkQueuePresentKHR spec notes
    {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT}, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, {0, 1, 0, 1});

  ctx.back.syncTrack.addImageAccess(
    // present barrier is very special, as vkQueuePresent does all visibility ops
    // ref: vkQueuePresentKHR spec notes
    {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT}, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, {0, 1, 0, 1});

  ctx.back.syncTrack.completeNeeded(ctx.frameCore, ctx.vkDev);
}

void Swapchain::makeReadyForPresent(ExecutionContext &ctx, Image *img)
{
  // handle case when we are not rendering, but presenting
  // fill screen with dummy black or last rendered image if it exists
  if (img->layout.get(0, 0) == VK_IMAGE_LAYOUT_UNDEFINED)
  {
    bool useDummy = (lastRenderedImage == nullptr) || (lastRenderedImage->layout.get(0, 0) == VK_IMAGE_LAYOUT_UNDEFINED);
    Image *fallbackImage = useDummy ? device.getDummy2DImage() : lastRenderedImage;
    doBlit(ctx, fallbackImage, img);
  }
  else if (lastRenderedImage)
    doBlit(ctx, img, lastRenderedImage);

  ctx.back.syncTrack.addImageAccess(
    // present barrier is very special, as vkQueuePresent does all visibility ops
    // ref: vkQueuePresentKHR spec notes
    {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_ACCESS_NONE}, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, {0, 1, 0, 1});

  ctx.back.syncTrack.completeNeeded(ctx.frameCore, ctx.vkDev);
}

void Swapchain::createLastRenderedImage(const VkSwapchainCreateInfoKHR &sci)
{
  G_ASSERTF(!lastRenderedImage, "vulkan: swapchain: should delete last rendered image before creating new");
  ImageCreateInfo ii;
  ii.type = VK_IMAGE_TYPE_2D;
  ii.mips = 1;
  ii.size.width = sci.imageExtent.width;
  ii.size.height = sci.imageExtent.height;
  ii.size.depth = 1;
  ii.arrays = 1;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // The swap images were created with a pure colorFormat without explicit srgb
  // so do the same
  auto formatList = queryPresentFormats();
  // match offscreen format with swapchain format if it is replaced due to format support
  if (!formatList.empty())
    activeMode.colorFormat = selectPresentFormat(formatList);
  ii.format = activeMode.colorFormat;
  ii.flags = activeMode.enableSrgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
  ii.residencyFlags = Image::MEM_NOT_EVICTABLE;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  lastRenderedImage = device.createImage(ii);
  device.setTexName(lastRenderedImage, "swapchain-last-rendered-image");
}

void Swapchain::destroyLastRenderedImage(FrameInfo &frame)
{
  if (lastRenderedImage)
  {
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*lastRenderedImage);
    lastRenderedImage = nullptr;
  }
}

void Swapchain::createOffscreenBuffer()
{
  ImageCreateInfo ii;
  ii.type = VK_IMAGE_TYPE_2D;
  ii.mips = 1;
  ii.size.width = activeMode.extent.width;
  ii.size.height = activeMode.extent.height;
  ii.size.depth = 1;
  ii.arrays = 1;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_SAMPLED_BIT;
  // The swap images were created with a pure colorFormat without explicit srgb
  // so do the same
  auto formatList = queryPresentFormats();
  // match offscreen format with swapchain format if it is replaced due to format support
  if (!formatList.empty())
    activeMode.colorFormat = selectPresentFormat(formatList);
  ii.format = activeMode.colorFormat;
  ii.flags = activeMode.enableSrgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
  ii.residencyFlags = Image::MEM_NOT_EVICTABLE;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  offscreenBuffer = device.createImage(ii);
  device.setTexName(offscreenBuffer, "swapchain-offscreen-buffer");
}

void Swapchain::destroyOffscreenBuffer(FrameInfo &frame)
{
  if (offscreenBuffer)
  {
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*offscreenBuffer);
    offscreenBuffer = nullptr;
  }
}

bool Swapchain::init(const SwapchainMode &initial_mode)
{
  frontMode = initial_mode;
  activeMode = initial_mode;
  handle = VulkanNullHandle();

#if _TARGET_ANDROID
  {
    char sdkVerS[PROP_VALUE_MAX + 1];
    int sdkVerI = 33;
    int sdkVerSLen = __system_property_get("ro.build.version.sdk", sdkVerS);
    if (sdkVerSLen >= 0)
      sdkVerI = atoi(sdkVerS);

    // on some exynos samsungs and Snapdragon 8 gen 1 chips with android 13
    // call to vkGetPhysicalDeviceSurfaceFormatsKHR with counts != 0
    // leaves some mutex acquired in gralloc stack, causing application to freeze
    // use format with 100% coverage instead of making a query for them

    // enable by default on all android 13 if not disabled by config
    // just in case if there is more such devices on market
    int minSdk = device.getPerDriverPropertyBlock("androidDeadlockAtQueryPresentFormats")->getInt("androidMinSdkVer", 33);
    usePredefinedPresentFormat = minSdk <= sdkVerI;
    if (usePredefinedPresentFormat)
      debug("vulkan: android: using predefined swapchain format for QueryPresentFormats deadlock workaround");
  }
#endif

  changeSwapState(SWP_HEADLESS);

  return true;
}

Image *Swapchain::getDepthStencilImageForExtent(VkExtent2D ext, FrameInfo &frame)
{
  if (depthStencilImage)
  {
    const auto &depthExt = depthStencilImage->getBaseExtent();
    if (depthExt.width >= ext.width && depthExt.height >= ext.height)
      return depthStencilImage;

    // adjust request to max of any
    ext.width = max(ext.width, depthExt.width);
    ext.height = max(ext.height, depthExt.height);

    // enqueue for deletion
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*depthStencilImage);

    // we should be aware of this - have a big potention to bug out frame
    debug("vulkan: swapchain depth image resize to [%u, %u]", ext.width, ext.height);
  }

  ImageCreateInfo ii;
  ii.type = VK_IMAGE_TYPE_2D;
  ii.format = activeMode.getDepthStencilFormat();
  ii.size.width = ext.width;
  ii.size.height = ext.height;
  ii.size.depth = 1;
  ii.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  ii.mips = 1;
  ii.arrays = 1;
  ii.flags = 0;
  ii.residencyFlags = Image::MEM_NOT_EVICTABLE;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  depthStencilImage = device.createImage(ii);
  device.setTexName(depthStencilImage, "swapchain-default-depth");
  return depthStencilImage;
}

void Swapchain::setMode(const SwapchainMode &new_mode)
{
  bool hasChanges = false;
  if (frontMode.surface != new_mode.surface)
  {
    debug("vulkan: swapchain mode change request: surface %016llX -> %016llX", generalize(frontMode.surface),
      generalize(new_mode.surface));
    hasChanges = true;
  }

  if (frontMode.extent != new_mode.extent)
  {
    debug("vulkan: swapchain mode change request: extent [%u, %u] -> [%u, %u]", frontMode.extent.width, frontMode.extent.height,
      new_mode.extent.width, new_mode.extent.height);
    hasChanges = true;
  }

  if (frontMode.presentMode != new_mode.presentMode)
  {
    debug("vulkan: swapchain mode change request: mode %u -> %u", (uint32_t)frontMode.presentMode, (uint32_t)new_mode.presentMode);
    hasChanges = true;
  }

  // only turn it on, never turn it off again
  if (frontMode.enableSrgb && !new_mode.enableSrgb)
  {
    debug("vulkan: swapchain mode change request: ignored srgb turnoff");
  }
  else if (frontMode.enableSrgb != new_mode.enableSrgb)
  {
    debug("vulkan: swapchain mode change request: enable srgb");
    hasChanges = true;
  }

  if (!hasChanges)
  {
    logwarn("vulkan: no changes in swapchain mode set");
    return;
  }

  frontMode = new_mode;
  DeviceContext &ctx = device.getContext();
  ctx.changeSwapchainMode(new_mode);
}

void Swapchain::preRotateStart(uint32_t offscreen_binding_idx, FrameInfo &frame, ExecutionContext &ctx)
{
  if (currentState == SWP_HEADLESS)
    return;
  G_ASSERT(currentState == SWP_DELAYED_ACQUIRE);

  // important to signal that app is wanting to process all rotations by itself
  // this will trigger all needed swapchain recreations
  preRotationProcessed = true;
  // do nothing if angle is zero, use blit instead
  if (!preRotationAngle)
    return;

  if (acquireSwapImage(frame))
  {
    colorTarget = swapImages[colorTargetIndex];

    ctx.imageBarrier(offscreenBuffer, ExecutionContext::BarrierImageType::Regular, RB_RO_SRV | RB_STAGE_PIXEL, 0, 0);
    auto &resBinds = ctx.back.pipelineState.getStageResourceBinds(ShaderStage::STAGE_PS);
    TRegister tReg;
    tReg.type = TRegister::TYPE_IMG;
    tReg.img.ptr = offscreenBuffer;
    auto replacementFormat = offscreenBuffer->getFormat();
    replacementFormat.isSrgb = activeMode.colorFormat.isSrgb && replacementFormat.isSrgbCapableFormatType();
    tReg.img.view.setFormat(replacementFormat);
    tReg.img.sampler = {};
    resBinds.set<StateFieldTRegisterSet, StateFieldTRegister::Indexed>({offscreen_binding_idx, tReg});
    savedOffscreenBinding = offscreen_binding_idx;

    changeSwapState(SWP_PRE_ROTATE);
  }
  else
  {
    destroyOffscreenBuffer(frame);
    changeSwapState(SWP_HEADLESS);
  }
}

void Swapchain::prePresent(ExecutionContext &ctx)
{
  if (currentState == SWP_DELAYED_ACQUIRE)
  {
    // do not blit to target if rotational transform is needed but was not performed
    // skip this frame and return to non-pre transform until app starts to render with transform
    //+ delay mode change if app is inactive
    if ((preRotationAngle > 0) && preRotation && !keepRotatedScreen)
    {
      preRotationProcessed = false;
      destroyOffscreenBuffer(ctx.back.contextState.frame.get());
      changeSwapState(SWP_HEADLESS);
      return;
    }

    if (acquireSwapImage(ctx.back.contextState.frame.get()))
    {
      if ((preRotationAngle > 0) && preRotation)
        keepRotatedScreen = false;
      else
      {
        // do not blit from offscreen buffer if it was not populated with content
        // followup code will handle this case filling swapchain image with dummy tex
        if (offscreenBuffer->layout.get(0, 0) != VK_IMAGE_LAYOUT_UNDEFINED)
          doBlit(ctx, offscreenBuffer, swapImages[colorTargetIndex]);
      }
      // tell the device we do not need the contents any more and make layout transition fast
      offscreenBuffer->layout.set(0, 0, VK_IMAGE_LAYOUT_UNDEFINED);
      changeSwapState(SWP_PRE_PRESENT);
    }
    else
    {
      destroyOffscreenBuffer(ctx.back.contextState.frame.get());
      changeSwapState(SWP_HEADLESS);
      return;
    }
  }

  if (currentState == SWP_PRE_ROTATE)
  {
    auto &resBinds = device.getContext().getBackend().pipelineState.getStageResourceBinds(ShaderStage::STAGE_PS);
    TRegister tReg;
    resBinds.set<StateFieldTRegisterSet, StateFieldTRegister::Indexed>({savedOffscreenBinding, tReg});

    colorTarget = offscreenBuffer;
    offscreenBuffer->layout.set(0, 0, VK_IMAGE_LAYOUT_UNDEFINED);
    changeSwapState(SWP_PRE_PRESENT);
  }

  if (currentState == SWP_PRE_PRESENT)
  {
    makeReadyForPresent(ctx, swapImages[colorTargetIndex]);
    changeSwapState(SWP_PRESENT);
  }
}

void Swapchain::present(ExecutionContext &ctx)
{
  G_ASSERT((currentState == SWP_HEADLESS) || (currentState == SWP_PRESENT));

  // signaled semaphores must be waited on to avoid bad reuse
  if (currentState == SWP_HEADLESS)
  {
    DeviceQueue::TrimmedSubmitInfo si = {};
    device.getQueue(DeviceQueueType::GRAPHICS).submit(vkDev, si);
  }

  if (currentState == SWP_PRESENT)
  {
    DeviceQueue::TrimmedPresentInfo pi;
    pi.swapchainCount = 1;
    pi.pSwapchains = ptr(handle);
    pi.pImageIndices = &colorTargetIndex;
    pi.enableSwappy = false;
#if ENABLE_SWAPPY
    pi.enableSwappy = swappyInitialized && swappyTarget != 0;
#endif

    VkResult result;
    {
      TIME_PROFILE(vulkan_swapchain_present);
      result = device.getQueue(DeviceQueueType::GRAPHICS).present(vkDev, pi);
    }

    if (checkVkSwapchainError(result, "vkQueuePresent"))
    {
      // if we not used offscreen buffer
      // clear target to prevent reusing wrong swapchain image
      if (!offscreenBuffer)
        colorTarget = nullptr;
      changeSwapState(offscreenBuffer ? SWP_DELAYED_ACQUIRE : SWP_EARLY_ACQUIRE);

#if _TARGET_ANDROID
      // only Android 10+ trigger suboptimals, on others we must poll (or do other stuff)
      if (preRotation)
      {
        ++surfaceRotationPolling;
        if ((surfaceRotationPolling % SURFACE_ROTATION_POLLING_INTERVAL) == 0 && preRotationProcessed && !is_null(activeMode.surface))
        {
          VkSurfaceCapabilitiesKHR caps = querySurfaceCaps();
          if (preRotationAngle != surfaceTransformToAngle(caps.currentTransform))
          {
            // should be ok to just drop into headless
            destroyOffscreenBuffer(ctx.back.contextState.frame.get());
            changeSwapState(SWP_HEADLESS);
          }
        }
      }
#endif
    }
    else
    {
      // we can have an offscreen buffer here,
      // so delete it before dropping to headless
      destroyOffscreenBuffer(ctx.back.contextState.frame.get());
      changeSwapState(SWP_HEADLESS);
    }
  }
}

int Swapchain::getPreRotationAngle() { return interlocked_relaxed_load(preRotationAngle); }

void Swapchain::shutdown(FrameInfo &frame)
{
  if (depthStencilImage)
  {
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*depthStencilImage);
    depthStencilImage = nullptr;
  }

  if (offscreenBuffer)
  {
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*offscreenBuffer);
    offscreenBuffer = nullptr;
  }

  destroyLastRenderedImage(frame);

  for (auto &&image : swapImages)
    frame.cleanups.enqueueFromBackend<Image::CLEANUP_DESTROY>(*image);
  swapImages.clear();

  colorTarget = nullptr;

  changeSwapState(SWP_INITIAL);
}

void Swapchain::onFrameBegin(ExecutionContext &ctx)
{
  if (currentState == SWP_HEADLESS)
  {
    if (device.getFeatures().test(DeviceFeaturesConfig::HEADLESS))
      return;

    // delay to avoid deleting in use object
    if (changeDelay)
    {
      --changeDelay;
      return;
    }

    ChangeResult ret = changeSwapchain(ctx.back.contextState.frame.get());
    if (ret == ChangeResult::OK)
    {
      changeConsequtiveFailures = 0;
      debug("vulkan: swapchain: presenting to display");
      changeSwapState(offscreenBuffer ? SWP_DELAYED_ACQUIRE : SWP_EARLY_ACQUIRE);
    }
    else
    {
      // headless mode is allowed for minimized state
      // also there can be some transient states
      if (ret != ChangeResult::RETRY)
      {
        ++changeConsequtiveFailures;
        // if we crash here, we must know what happened
        // logerr on limit and crash on limit * 2, so error is sent to stats or saved to crash
        if (changeConsequtiveFailures == changeConsequtiveFailuresLimit)
          logerr("vulkan: can't init swapchain");
        if (changeConsequtiveFailures > changeConsequtiveFailuresLimit * 2)
          fatal("vulkan: can't init swapchain, crash");
      }
    }
  }

  if (currentState == SWP_EARLY_ACQUIRE)
  {
    if (acquireSwapImage(ctx.back.contextState.frame.get()))
    {
      colorTarget = swapImages[colorTargetIndex];
      changeSwapState(SWP_PRE_PRESENT);
    }
    else
      changeSwapState(SWP_HEADLESS);
  }
}

void Swapchain::setSwappyTargetFrameRate(int rate)
{
  G_UNUSED(rate);

#if ENABLE_SWAPPY
  swappyTarget = rate;
  if (swappyTarget == 0 || !swappyInitialized)
    return;

  uint64_t swappy_ns = 1000000000L / swappyTarget;

  debug("vulkan: Swappy target swap rate is %llu ns", swappy_ns);
  SwappyVk_setSwapIntervalNS(device.getDevice(), handle, swappy_ns);
#endif
}

void Swapchain::getSwappyStatus(int *status)
{
  G_UNUSED(status);

#if ENABLE_SWAPPY
  const int v = swappyInitialized && (swappyTarget != 0) ? 1 : 0;
  interlocked_relaxed_store(*status, v);
#else
  *status = 0;
#endif
}

void Swapchain::changeSwapchainMode(ExecutionContext &ctx, const SwapchainMode &new_mode)
{
  // ignore mode change if active mode matches it
  if (activeMode.compare(new_mode))
    return;

  // image was acquired - we can't reuse swapchain with acquired image
  // and presenting it when it is not ready will show user some garbadge
  // need to remove swapchain
  bool imageAcquired = (currentState == SWP_PRE_PRESENT) || (currentState == SWP_PRESENT);
  // surface changes - we can't remove old surface while swapchain is active
  // need to remove swapchain too
  bool surfaceChanged = (activeMode.surface != new_mode.surface);

  if (imageAcquired || surfaceChanged)
  {
    debug("vulkan: swapchain: special mode change due to: %s %s", imageAcquired ? "acquired image" : "",
      surfaceChanged ? "changed surface" : "");
    // no need to delay change of swapchain, we waited already
    changeDelay = 0;

    // remove swapchain right now
    // leftovers will be cleaned up in changeSwapchain
    destroySwapchainHandle(handle);
  }

  // destroy old surface if needed
  if (surfaceChanged)
  {
    VulkanInstance &inst = vkDev.getInstance();
    VULKAN_LOG_CALL(inst.vkDestroySurfaceKHR(inst.get(), activeMode.surface, nullptr));
    activeMode.surface = VulkanNullHandle();
  }

  activeMode = new_mode;

  // drop to headless
  // swapchain will be restored lazily with new parameters
  destroyOffscreenBuffer(ctx.back.contextState.frame.get());
  changeSwapState(SWP_HEADLESS);
}

void Swapchain::changeSwapState(SwapStateId new_state)
{
  // only certain state changes are allowed
  G_ASSERTF(
    // can drop to headless from anything
    (new_state == SWP_HEADLESS) || ((currentState == SWP_HEADLESS) && (new_state == SWP_DELAYED_ACQUIRE)) ||
      ((currentState == SWP_HEADLESS) && (new_state == SWP_EARLY_ACQUIRE)) ||
      ((currentState == SWP_HEADLESS) && (new_state == SWP_INITIAL)) ||
      ((currentState == SWP_DELAYED_ACQUIRE) && (new_state == SWP_PRE_PRESENT)) ||
      ((currentState == SWP_DELAYED_ACQUIRE) && (new_state == SWP_PRE_ROTATE)) ||
      ((currentState == SWP_PRE_ROTATE) && (new_state == SWP_PRE_PRESENT)) ||
      ((currentState == SWP_EARLY_ACQUIRE) && (new_state == SWP_PRE_PRESENT)) ||
      ((currentState == SWP_PRE_PRESENT) && (new_state == SWP_PRESENT)) ||
      ((currentState == SWP_PRESENT) && (new_state == SWP_DELAYED_ACQUIRE)) ||
      ((currentState == SWP_PRESENT) && (new_state == SWP_EARLY_ACQUIRE)),
    "vulkan: swapchain: bad state change %u -> %u", currentState, new_state);

  currentState = new_state;

  // dropping to headless is in common drop to new offscreen buffer
  if (currentState == SWP_HEADLESS)
  {
    // we should delete any buffer that was here before dropping down to headless
    G_ASSERT(!offscreenBuffer);
    createOffscreenBuffer();
    colorTarget = offscreenBuffer;
    debug("vulkan: swapchain: running headlessly");
  }

  validateSwapState();
}

void Swapchain::validateSwapState()
{
#if DAGOR_DBGLEVEL > 0
  // debug grade checks here
  // if something is out of order, we catch it here,
  // not inside detail-lacking(!!!) gpu/driver/vulkan api crashes
  if (currentState == SWP_INITIAL)
  {
    G_ASSERT(is_null(handle));
    G_ASSERT(!offscreenBuffer);
    G_ASSERT(is_null(activeMode.surface));
    G_ASSERT(!colorTarget);
    G_ASSERT(!swapImages.size());
  }
  else if (currentState == SWP_HEADLESS)
  {
    G_ASSERT(offscreenBuffer);
    G_ASSERT(offscreenBuffer->getBaseExtent().width == activeMode.extent.width);   // -V522
    G_ASSERT(offscreenBuffer->getBaseExtent().height == activeMode.extent.height); // -V522
    G_ASSERT(colorTarget == offscreenBuffer);
  }
  else if (currentState == SWP_DELAYED_ACQUIRE)
  {
    G_ASSERT(!is_null(handle));
    G_ASSERT(!is_null(activeMode.surface));
    G_ASSERT(offscreenBuffer);
    G_ASSERT(offscreenBuffer->getBaseExtent().width == activeMode.extent.width);   // -V522
    G_ASSERT(offscreenBuffer->getBaseExtent().height == activeMode.extent.height); // -V522
    G_ASSERT(colorTarget == offscreenBuffer);
    G_ASSERT(swapImages.size());
  }
  else if (currentState == SWP_EARLY_ACQUIRE)
  {
    G_ASSERT(!is_null(handle));
    G_ASSERT(!is_null(activeMode.surface));
    G_ASSERT(!offscreenBuffer);
    G_ASSERT(!colorTarget);
    G_ASSERT(swapImages.size());
    G_ASSERT(swapImages[0]->getBaseExtent().width == activeMode.extent.width);
    G_ASSERT(swapImages[0]->getBaseExtent().height == activeMode.extent.height);
    G_ASSERT(!activeMode.enableSrgb);
  }
  else if ((currentState == SWP_PRE_PRESENT) || (currentState == SWP_PRESENT))
  {
    bool isTargetFromSwapList = false;
    for (int i = 0; i < swapImages.size(); ++i)
      isTargetFromSwapList |= swapImages[i] == colorTarget;

    G_ASSERT(isTargetFromSwapList != (colorTarget == offscreenBuffer));
    G_ASSERT(colorTarget);
    G_ASSERT(!is_null(handle));
    G_ASSERT(!is_null(activeMode.surface));
    G_ASSERT(swapImages.size());
  }
#endif
}
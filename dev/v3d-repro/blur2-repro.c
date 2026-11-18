/*
 * blur2-repro: standalone Vulkan reproducer for the SECOND V3D 7.1 (Raspberry Pi 5) GPU hang (README item 124).
 *
 * It draws DWM's shadow-chain blur (pipeline 32103 of the capture: the real DXVK vertex and fragment shaders
 * shaders/blur2_{vs,fs}.spv, 8 texture samples, 7 varyings) as two triangles (6 vertices) with
 * three vec4 vertex attributes into a 512x512 BGRA8 image, sampling a second 512x512 image (ping-pong),
 * with the 96-byte uniform buffers of the hanging draw.
 *
 *   ./blur2-repro [--iters N] [--spv-dir DIR] [--secondary] [-v]
 *
 * A submit slower than 300 ms is reported as SLOW; check `dmesg | grep -c 'Resetting GPU'` before/after.
 * Mesa switches (the patched Mesa in ~/bvm-gpu): BVM_V3D_LOOPTEX_2T=0 turns the workaround OFF (needed
 * to see the hang with that Mesa), V3D_DEBUG=shaderdb prints the thread count of the shader.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>


#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;
static VkPhysicalDeviceMemoryProperties memprops;

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "no memory type\n"); exit(1);
}

static void *read_file(const char *dir, const char *name, size_t *size)
{
   char path[1024];
   if (name[0] == '/') snprintf(path, sizeof(path), "%s", name);
   else snprintf(path, sizeof(path), "%s/%s", dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { perror(path); exit(1); }
   fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
   void *p = malloc(*size);
   if (fread(p, 1, *size, f) != *size) { perror("read"); exit(1); }
   fclose(f);
   return p;
}

static VkShaderModule load_shader(const char *dir, const char *name)
{
   size_t sz; void *code = read_file(dir, name, &sz);
   VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
   VkShaderModule m; VK(vkCreateShaderModule(dev, &ci, NULL, &m)); free(code); return m;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
   VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   VkBuffer b; VK(vkCreateBuffer(dev, &bi, NULL, &b));
   VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
   VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
   VkDeviceMemory m; VK(vkAllocateMemory(dev, &ai, NULL, &m));
   VK(vkBindBufferMemory(dev, b, m, 0));
   VK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
   return b;
}

static double now_ms(void)
{
   struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
   const char *spv = NULL; const char *fsname = "blur2_fs.spv"; int iters = 2000; int secondary = 0, verbose = 0;
   char defdir[512]; snprintf(defdir, sizeof(defdir), "%s/bvm/dev/v3d-repro/shaders", getenv("HOME") ? getenv("HOME") : ".");
   spv = defdir;
   for (int i = 1; i < argc; i++) {
      if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = atoi(argv[++i]);
      else if (!strcmp(argv[i], "--spv-dir") && i + 1 < argc) spv = argv[++i];
      else if (!strcmp(argv[i], "--fs") && i + 1 < argc) fsname = argv[++i];
      else if (!strcmp(argv[i], "--secondary")) secondary = 1;
      else if (!strcmp(argv[i], "-v")) verbose = 1;
      else { fprintf(stderr, "usage: %s [--iters N] [--spv-dir DIR] [--secondary] [-v]\n", argv[0]); return 2; }
   }

   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst; VK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 0; VK(vkEnumeratePhysicalDevices(inst, &n, NULL));
   VkPhysicalDevice pds[8]; if (n > 8) n = 8; VK(vkEnumeratePhysicalDevices(inst, &n, pds));
   phys = pds[0];
   for (uint32_t i = 0; i < n; i++) { VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p); if (strstr(p.deviceName, "V3D")) phys = pds[i]; }
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
   printf("device: %s (api %u.%u.%u, driver 0x%x)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
          VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
   vkGetPhysicalDeviceMemoryProperties(phys, &memprops);

   VkPhysicalDeviceFeatures supported; vkGetPhysicalDeviceFeatures(phys, &supported);
   /* DXVK enables robustBufferAccess; it makes Mesa add bounds checks to every UBO read (the
    * replay's shader is 194 instructions with them, 121 without). --no-robust turns it off. */
   VkPhysicalDeviceFeatures feats = { .depthClamp = supported.depthClamp,
      .robustBufferAccess = supported.robustBufferAccess && !getenv("BLUR_NO_ROBUST") };
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
   /* DXVK also enables robustImageAccess (Vulkan 1.3 feature); it changes how Mesa compiles texture reads.
    * BLUR_NO_ROBUSTIMG=1 turns it off. */
   VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
      .robustImageAccess = getenv("BLUR_NO_ROBUSTIMG") ? VK_FALSE : VK_TRUE };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &feats };
   VK(vkCreateDevice(phys, &dci, NULL, &dev));
   VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);

   /* two 512x512 BGRA8 images, 1 MB each, sub-allocated from one memory block like DXVK does */
   VkImage img[2]; VkImageView view[2];
   VkImageCreateInfo imci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { 512, 512, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkMemoryRequirements imr = { 0 };
   for (int i = 0; i < 2; i++) { VK(vkCreateImage(dev, &imci, NULL, &img[i])); vkGetImageMemoryRequirements(dev, img[i], &imr); }
   VkDeviceSize stride = (imr.size + imr.alignment - 1) / imr.alignment * imr.alignment;
   VkMemoryAllocateInfo imai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = stride * 2 + (4u << 20),
      .memoryTypeIndex = find_mem(imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   VkDeviceMemory imem; VK(vkAllocateMemory(dev, &imai, NULL, &imem));
   for (int i = 0; i < 2; i++) {
      VK(vkBindImageMemory(dev, img[i], imem, (4u << 20) / 2 + stride * i));
      VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = VK_FORMAT_B8G8R8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
      VK(vkCreateImageView(dev, &vci, NULL, &view[i]));
   }

   /* uniform / vertex / staging data */
   void *map;
   VkBuffer ubo = make_buffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &map);
   memset(map, 0, 4096);
   /* cb0 of the vertex shader (set 2 binding 0) and of the fragment shader (set 2 binding 1), 96 bytes each,
    * as captured from the hanging draw: tap count 4, then offset/weight pairs of a Gaussian kernel */
   static const uint32_t ubo_data[24] = {
      0x00000004, 0x40000000, 0x3f800000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x3f269ef5, 0x00000000, 0x3e5c6b1c, 0x00000000, 0x401a6ea7, 0x00000000, 0x3e45ebe8, 0x00000000,
      0x408b1996, 0x00000000, 0x3d990305, 0x00000000, 0x40c9284c, 0x00000000, 0x3c893bd1, 0x00000000 };
   memcpy((char *)map + 0, ubo_data, sizeof(ubo_data));
   memcpy((char *)map + 256, ubo_data, sizeof(ubo_data));
   memcpy((char *)map + 512, ubo_data, sizeof(ubo_data));
   /* three vec4 vertex attributes (locations 0..2), 6 vertices (two triangles) */
   VkBuffer vbo = make_buffer(1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &map);
   const float pos[6][4] = { { -1, -1, 0, 1 }, { 1, -1, 0, 1 }, { -1, 1, 0, 1 }, { 1, -1, 0, 1 }, { 1, 1, 0, 1 }, { -1, 1, 0, 1 } };
   const float at1[6][4] = { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 1, 0, 0, 1 }, { 1, 1, 0, 1 }, { 0, 1, 0, 1 } };
   const float at2[6][4] = { { 0, 0, 0, 0 }, { 0.5f, 0, 0, 0 }, { 0, 0.5f, 0, 0 }, { 0.5f, 0, 0, 0 }, { 0.5f, 0.5f, 0, 0 }, { 0, 0.5f, 0, 0 } };
   memcpy((char *)map + 0, pos, sizeof(pos));
   memcpy((char *)map + 256, at1, sizeof(at1));
   memcpy((char *)map + 512, at2, sizeof(at2));
   VkBuffer stg = make_buffer(512 * 512 * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &map);
   for (int i = 0; i < 512 * 512; i++) ((uint32_t *)map)[i] = 0xff000000u | (uint32_t)((i * 2654435761u) >> 8);

   /* descriptor sets: 0 = sampler heap, 1 = sampled image (one per ping-pong source), 2 = UBOs */
   VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutBinding b1 = { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
   VkDescriptorSetLayoutBinding b2[3] = { { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
      { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
      { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT } };
   VkDescriptorSetLayout dsl[3];
   VkDescriptorSetLayoutCreateInfo dli[3] = {
      { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0 },
      { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b1 },
      { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b2 } };
   for (int i = 0; i < 3; i++) VK(vkCreateDescriptorSetLayout(dev, &dli[i], NULL, &dsl[i]));
   VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3, .pSetLayouts = dsl,
      .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
   VkPipelineLayout pl; VK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));

   VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 } };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 3, .pPoolSizes = ps };
   VkDescriptorPool pool; VK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
   VkDescriptorSetLayout sl[4] = { dsl[0], dsl[1], dsl[1], dsl[2] };
   VkDescriptorSet sets[4];
   VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool, .descriptorSetCount = 4, .pSetLayouts = sl };
   VK(vkAllocateDescriptorSets(dev, &dai, sets));
   /* sampler: linear, clamp to edge, lod 0..15 (as in the capture) */
   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .minLod = 0, .maxLod = 15, .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
   VkSampler smp; VK(vkCreateSampler(dev, &sci, NULL, &smp));
   VkDescriptorImageInfo sinfo[4]; for (int i = 0; i < 4; i++) sinfo[i] = (VkDescriptorImageInfo){ .sampler = smp };
   VkDescriptorImageInfo iinfo[2] = { { .imageView = view[0], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                      { .imageView = view[1], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
   VkDescriptorBufferInfo binfo[3] = { { ubo, 0, 96 }, { ubo, 256, 96 }, { ubo, 512, 96 } };
   VkWriteDescriptorSet w[6] = {
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 0, .descriptorCount = 4, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = sinfo },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &iinfo[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &iinfo[1] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[3], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &binfo[0] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[3], .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &binfo[1] },
      { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[3], .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &binfo[2] } };
   vkUpdateDescriptorSets(dev, 6, w, 0, NULL);

   /* render pass: one BGRA8 attachment, LOAD/STORE, stays in SHADER_READ_ONLY between passes */
   VkAttachmentDescription ad = { .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ar };
   VkSubpassDependency deps[2] = {
      { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
      { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 } };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1, .pAttachments = &ad, .subpassCount = 1,
      .pSubpasses = &sub, .dependencyCount = 2, .pDependencies = deps };
   VkRenderPass rp; VK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebuffer fb[2];
   for (int i = 0; i < 2; i++) {
      VkFramebufferCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 1, .pAttachments = &view[i], .width = 512, .height = 512, .layers = 1 };
      VK(vkCreateFramebuffer(dev, &fi, NULL, &fb[i]));
   }

   /* pipeline */
   VkShaderModule vs = load_shader(spv, "blur2_vs.spv"), fs = load_shader(spv, fsname);
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkVertexInputBindingDescription vb[3] = { { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 16, VK_VERTEX_INPUT_RATE_VERTEX }, { 2, 16, VK_VERTEX_INPUT_RATE_VERTEX } };
   VkVertexInputAttributeDescription va[3] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 2, 2, VK_FORMAT_R32G32B32A32_SFLOAT, 0 } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 3,
      .pVertexBindingDescriptions = vb, .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = va };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = feats.depthClamp,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
   VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
      .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
      .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe; VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
   VkCommandPool cp; VK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd; VK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence; VK(vkCreateFence(dev, &fci, NULL, &fence));

   /* initial contents: upload the staging image into both images, leave them SHADER_READ_ONLY */
   {
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
      VK(vkBeginCommandBuffer(cmd, &bi));
      for (int i = 0; i < 2; i++) {
         VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img[i], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 512, 512, 1 } };
         vkCmdCopyBufferToImage(cmd, stg, img[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
         b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      }
      VK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      VK(vkQueueSubmit(queue, 1, &si, fence));
      VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)); VK(vkResetFences(dev, 1, &fence));
   }

   printf("running %d passes (%s)\n", iters, secondary ? "secondary cmd buffer inside the pass" : "draw in the primary");
   int slow = 0; double worst = 0, total = 0;
   VkCommandBuffer sec = VK_NULL_HANDLE;
   if (secondary) {
      VkCommandBufferAllocateInfo sai = cai; sai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
      VK(vkAllocateCommandBuffers(dev, &sai, &sec));
   }
   for (int it = 0; it < iters; it++) {
      int dst = it & 1, src = dst ^ 1;
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
      VK(vkResetCommandBuffer(cmd, 0)); VK(vkBeginCommandBuffer(cmd, &bi));
      VkRenderPassBeginInfo rb = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb[dst], .renderArea = { { 0, 0 }, { 512, 512 } } };
      VkViewport viewport = { 0, 512, 512, -512, 0, 1 }; VkRect2D scissor = { { 0, 0 }, { 512, 512 } };
      VkBuffer vbs[3] = { vbo, vbo, vbo }; VkDeviceSize vbo_off[3] = { 0, 256, 512 };
      VkCommandBuffer rec = cmd;
      if (secondary) {
         VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, .renderPass = rp, .subpass = 0, .framebuffer = fb[dst] };
         VkCommandBufferBeginInfo sbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, .pInheritanceInfo = &inh };
         VK(vkResetCommandBuffer(sec, 0)); VK(vkBeginCommandBuffer(sec, &sbi)); rec = sec;
      }
      VkDescriptorSet bind[3] = { sets[0], sets[1 + src], sets[3] };
      uint32_t pc = 0;
      if (secondary) {
         vkCmdBindPipeline(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         vkCmdBindDescriptorSets(rec, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 3, bind, 0, NULL);
         vkCmdBindVertexBuffers(rec, 0, 3, vbs, vbo_off);
         vkCmdSetViewport(rec, 0, 1, &viewport); vkCmdSetScissor(rec, 0, 1, &scissor);
         vkCmdPushConstants(rec, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &pc);
         vkCmdDraw(rec, 6, 1, 0, 0);
      }
      if (secondary) {
         VK(vkEndCommandBuffer(sec));
         vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
         vkCmdExecuteCommands(cmd, 1, &sec);
      } else {
         vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
         /* primary: nothing recorded before the pass, so re-record the draw here */
         vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 3, bind, 0, NULL);
         vkCmdBindVertexBuffers(cmd, 0, 3, vbs, vbo_off);
         vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);
         vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &pc);
         vkCmdDraw(cmd, 6, 1, 0, 0);
      }
      vkCmdEndRenderPass(cmd);
      VK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      double t0 = now_ms();
      VK(vkQueueSubmit(queue, 1, &si, fence));
      VK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX)); VK(vkResetFences(dev, 1, &fence));
      double ms = now_ms() - t0; total += ms; if (ms > worst) worst = ms;
      if (ms > 300) { slow++; printf("SLOW pass %d: %.0f ms\n", it, ms); fflush(stdout); }
      else if (verbose && it < 5) printf("pass %d: %.2f ms\n", it, ms);
   }
   printf("done: %d slow passes (> 300 ms), worst %.1f ms, avg %.2f ms\n", slow, worst, total / iters);
   printf("now compare: sudo dmesg | grep -c 'Resetting GPU'\n");
   return 0;
}

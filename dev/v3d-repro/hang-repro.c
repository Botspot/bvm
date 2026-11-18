/*
 * hang-repro.c - standalone Vulkan reproducer for the V3D (Raspberry Pi 5) GPU hang
 * "v3d ... Resetting GPU for hang" seen with the Triton/DXVK/V3DV stack (see README.md).
 *
 * What it does: records and submits the same kind of work as the guest's DWM: chains of render
 * passes that LOAD and STORE a small BGRA8 attachment and draw NOTHING (frame sizes like 120x8 on
 * a 128x16 image, 512x512, ...), with barriers between them, optionally preceded by image copies,
 * while optionally other processes keep drawing (like labwc / the VM window blit).  Every submit is
 * timed; a submit that takes > 300 ms is reported (the kernel resets the GPU after ~1 s).
 *
 * No Windows, no DXVK, no virglrenderer: plain Vulkan 1.0 on the stock Mesa V3DV driver.
 * Build:  make        Run:  ./hang-repro --help
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
   fprintf(stderr, "%s:%d: %s failed: %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ shapes */
struct shape {
   uint32_t fw, fh;     /* frame (render area / framebuffer) size */
   uint32_t iw, ih;     /* image size */
   int slot;            /* image identity: same slot = same image */
   bool clear;          /* loadOp CLEAR instead of LOAD */
   bool copy_before;    /* vkCmdCopyImage from a scratch image into this one first */
   bool draw;           /* record one tiny draw inside the pass */
   bool sec;            /* contents come from a SECONDARY command buffer (one draw), like DXVK on tilers */
   bool sec_empty;      /* secondary command buffer with no commands at all */
   bool tex;            /* secondary with a draw that samples the PREVIOUS pass's image + vertex buffer + UBO
                         * (the DWM blur/downsample chain); implies sec */
   bool split;          /* draw, then a pipeline barrier INSIDE the render pass, then end the pass:
                         * V3DV splits the job at the barrier, the second job is an empty LOAD/STORE
                         * job with ser=7 bclsync=1 = what the guest's hanging jobs look like */
};

#define MAX_SHAPES 64
static struct shape shapes[MAX_SHAPES];
static int nshapes;

/* syntax: FWxFH[@IWxIH][#slot][!c][!y]   (!c = clear, !y = copy before) */
static int parse_shapes(const char *s)
{
   nshapes = 0;
   while (*s && nshapes < MAX_SHAPES) {
      struct shape sh = { 0 };
      int n = 0;
      if (sscanf(s, "%ux%u%n", &sh.fw, &sh.fh, &n) < 2) return -1;
      s += n;
      sh.iw = sh.fw; sh.ih = sh.fh;
      sh.slot = -1;
      while (*s && *s != ',') {
         if (*s == '@') { s++; if (sscanf(s, "%ux%u%n", &sh.iw, &sh.ih, &n) < 2) return -1; s += n; }
         else if (*s == '#') { s++; if (sscanf(s, "%d%n", &sh.slot, &n) < 1) return -1; s += n; }
         else if (*s == '!') {
            s++;
            if (*s == 'c') sh.clear = true;
            else if (*s == 'y') sh.copy_before = true;
            else if (*s == 'd') sh.draw = true;
            else if (*s == 'b') sh.split = true;
            else if (*s == 's') sh.sec = true;
            else if (*s == 'e') sh.sec_empty = true;
            else if (*s == 't') { sh.sec = true; sh.tex = true; }
            else return -1;
            s++;
         }
         else return -1;
      }
      if (*s == ',') s++;
      if (sh.slot < 0) sh.slot = 1000 + nshapes;
      shapes[nshapes++] = sh;
   }
   return nshapes ? 0 : -1;
}

/* Chains read from the guest's JOBLOG (README items 105/106, run-hang3 / run-tiny2). */
static const char *SCEN_YT =        /* DWM LOAD/STORE chain ending in the 120x8 pair that hung every time */
   "1920x970#0,2048x128#1,1024x64#2,512x32#3,256x16#4,128x16#5,120x8@128x16#6,120x8@128x16#5,1897x112@1920x970#7";
static const char *SCEN_SHADOW =    /* DWM shadow/blur chain at the first tooltip (512x512 pair hung) */
   "80x80#10!c,128x128#11!c,128x128#12!c,512x512#13,512x512#14,512x512#13,80x80#10,64x64#15!c,64x64#16!c,64x64#17!c";
static const char *SCEN_SMALL =     /* just the 120x8-on-128x16 pair, nothing else */
   "128x16#5,120x8@128x16#6,120x8@128x16#5";
/* "-b" variants: every pass draws once and then has a pipeline barrier INSIDE the render pass, so V3DV
 * splits the job and the second job is the empty ser=7 bclsync=1 LOAD/STORE job seen in the guest log
 * (README item 112).  These are the ones that look like the guest's hanging jobs. */
static const char *SCEN_YT_B =
   "1920x970#0!b,2048x128#1!b,1024x64#2!b,512x32#3!b,256x16#4!b,128x16#5!b,120x8@128x16#6!b,120x8@128x16#5!b,1897x112@1920x970#7!b";
static const char *SCEN_SHADOW_B =
   "80x80#10!c!b,128x128#11!c!b,128x128#12!c!b,512x512#13!b,512x512#14!b,512x512#13!b,80x80#10!b,64x64#15!c!b,64x64#16!c!b,64x64#17!c!b";
static const char *SCEN_SMALL_B =
   "128x16#5!b,120x8@128x16#6!b,120x8@128x16#5!b";

/* ------------------------------------------------------------------ vulkan basics */
struct vk {
   VkInstance inst;
   VkPhysicalDevice phys;
   VkDevice dev;
   VkQueue q;
   uint32_t qfam;
   VkPhysicalDeviceMemoryProperties mp;
   char name[512];
};

static void vk_init(struct vk *v, bool verbose)
{
   VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "hang-repro",
                            .apiVersion = VK_API_VERSION_1_0 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   CHECK(vkCreateInstance(&ici, NULL, &v->inst));
   uint32_t n = 0;
   CHECK(vkEnumeratePhysicalDevices(v->inst, &n, NULL));
   VkPhysicalDevice pd[8];
   if (n > 8) n = 8;
   CHECK(vkEnumeratePhysicalDevices(v->inst, &n, pd));
   v->phys = VK_NULL_HANDLE;
   for (uint32_t i = 0; i < n; i++) {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(pd[i], &p);
      if (verbose) fprintf(stderr, "device %u: %s\n", i, p.deviceName);
      if (strstr(p.deviceName, "V3D") && !v->phys) {
         v->phys = pd[i];
         snprintf(v->name, sizeof v->name, "%s (api %u.%u.%u, driver 0x%x)", p.deviceName,
                  VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                  VK_VERSION_PATCH(p.apiVersion), p.driverVersion);
      }
   }
   if (!v->phys) { fprintf(stderr, "no V3D Vulkan device found (set VK_ICD_FILENAMES?)\n"); exit(1); }
   vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mp);
   uint32_t nq = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, NULL);
   VkQueueFamilyProperties qp[8];
   if (nq > 8) nq = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(v->phys, &nq, qp);
   v->qfam = UINT32_MAX;
   for (uint32_t i = 0; i < nq; i++)
      if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { v->qfam = i; break; }
   if (v->qfam == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); exit(1); }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = v->qfam, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   CHECK(vkCreateDevice(v->phys, &dci, NULL, &v->dev));
   vkGetDeviceQueue(v->dev, v->qfam, 0, &v->q);
}

static uint32_t find_mem(const struct vk *v, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (v->mp.memoryTypes[i].propertyFlags & want) == want) return i;
   for (uint32_t i = 0; i < v->mp.memoryTypeCount; i++)
      if (bits & (1u << i)) return i;
   fprintf(stderr, "no memory type\n");
   exit(1);
}

/* ------------------------------------------------------------------ images */
struct img {
   int slot;
   uint32_t w, h;
   VkImage image;
   VkImageView view;
   VkDeviceMemory dedicated;    /* if not suballocated */
   VkDeviceSize heap_off;
   bool inited;
};

#define MAX_IMGS 64
static struct img imgs[MAX_IMGS];
static int nimgs;
static VkDeviceMemory heap;
static VkDeviceSize heap_size, heap_used;
static VkImage scratch;           /* copy source */

static struct img *get_img(struct vk *v, const struct shape *sh)
{
   for (int i = 0; i < nimgs; i++)
      if (imgs[i].slot == sh->slot) return &imgs[i];
   if (nimgs >= MAX_IMGS) { fprintf(stderr, "too many images\n"); exit(1); }
   struct img *im = &imgs[nimgs++];
   im->slot = sh->slot; im->w = sh->iw; im->h = sh->ih;
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM, .extent = { sh->iw, sh->ih, 1 }, .mipLevels = 1, .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   CHECK(vkCreateImage(v->dev, &ici, NULL, &im->image));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(v->dev, im->image, &mr);
   if ((uint64_t)sh->iw * sh->ih > 512 * 512) {              /* big images get their own allocation like DXVK */
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = find_mem(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      CHECK(vkAllocateMemory(v->dev, &mai, NULL, &im->dedicated));
      CHECK(vkBindImageMemory(v->dev, im->image, im->dedicated, 0));
   } else {
      VkDeviceSize off = (heap_used + mr.alignment - 1) / mr.alignment * mr.alignment;
      if (off + mr.size > heap_size) { fprintf(stderr, "heap too small\n"); exit(1); }
      CHECK(vkBindImageMemory(v->dev, im->image, heap, off));
      im->heap_off = off;
      heap_used = off + mr.size;
   }
   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   CHECK(vkCreateImageView(v->dev, &vci, NULL, &im->view));
   return im;
}

static VkRenderPass make_rp(struct vk *v, bool clear)
{
   VkAttachmentDescription ad = {
      .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &ref };
   /* self-dependency so a pipeline barrier can be recorded inside the pass (DXVK does this for
    * feedback / UAV hazards); the dst stages include the binning stages so V3DV flags a bin sync */
   VkSubpassDependency dep = {
      .srcSubpass = 0, .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
      .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT };
   VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                  .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp,
                                  .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(v->dev, &rci, NULL, &rp));
   return rp;
}

static void inpass_barrier(VkCommandBuffer cb)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                       VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT };
   vkCmdPipelineBarrier(cb,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
}

static void barrier_all(VkCommandBuffer cb)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void image_barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ------------------------------------------------------------------ background load process */
static const char *spv_dir;

static VkShaderModule load_spv(struct vk *v, const char *name)
{
   char path[1024];
   snprintf(path, sizeof path, "%s/%s", spv_dir, name);
   FILE *f = fopen(path, "rb");
   if (!f) { fprintf(stderr, "cannot open %s (run `make shaders`)\n", path); exit(1); }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   uint32_t *code = malloc(sz);
   if (fread(code, 1, sz, f) != (size_t)sz) exit(1);
   fclose(f);
   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sz, .pCode = code };
   VkShaderModule m;
   CHECK(vkCreateShaderModule(v->dev, &smi, NULL, &m));
   free(code);
   return m;
}

/* Pipeline with dynamic viewport/scissor that draws one full-frame triangle (for the !d / !b shapes). */
static VkPipeline make_tri_pipeline(struct vk *v, VkRenderPass rp)
{
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(v->dev, &plci, NULL, &pl));
   VkShaderModule vs = load_spv(v, "tri.vert.spv"), fs = load_spv(v, "tri.frag.spv");
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba };
   VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2, .pDynamicStates = dyns };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
   return pipe;
}

/* Textured pipeline: vertex buffer (pos, uv), set 0 = {sampler2D, UBO tint}, dynamic viewport/scissor. */
static VkDescriptorSetLayout tex_dsl;
static VkPipelineLayout tex_pl;

static VkPipeline make_tex_pipeline(struct vk *v, VkRenderPass rp)
{
   VkDescriptorSetLayoutBinding b[2] = {
      { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
      { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2, .pBindings = b };
   CHECK(vkCreateDescriptorSetLayout(v->dev, &dli, NULL, &tex_dsl));
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &tex_dsl };
   CHECK(vkCreatePipelineLayout(v->dev, &plci, NULL, &tex_pl));
   VkShaderModule vs = load_spv(v, "tex.vert.spv"), fs = load_spv(v, "tex.frag.spv");
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkVertexInputBindingDescription vb = { .binding = 0, .stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
   VkVertexInputAttributeDescription va[2] = {
      { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
      { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vb,
      .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .scissorCount = 1 };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba };
   VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
   VkPipelineDynamicStateCreateInfo dsi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2, .pDynamicStates = dyns };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dsi, .layout = tex_pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(v->dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
   return pipe;
}

/* Draws full-screen triangles (a compositor-like load: real bin jobs with draws) until killed. */
static void bg_main(int idx, int frame_sleep_us)
{
   struct vk v = { 0 };
   vk_init(&v, false);
   const uint32_t W = 1280, H = 720;
   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img;
   CHECK(vkCreateImage(v.dev, &ici, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(v.dev, img, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = find_mem(&v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(v.dev, &mai, NULL, &mem));
   CHECK(vkBindImageMemory(v.dev, img, mem, 0));
   VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
      .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView view;
   CHECK(vkCreateImageView(v.dev, &vci, NULL, &view));
   /* render pass: clear -> store, initial layout undefined (we discard the old content) */
   VkAttachmentDescription ad = { .format = VK_FORMAT_B8G8R8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref };
   VkRenderPassCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
      .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sp };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(v.dev, &rci, NULL, &rp));
   VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
      .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(v.dev, &fci, NULL, &fb));
   VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(v.dev, &plci, NULL, &pl));
   VkShaderModule vs = load_spv(&v, "tri.vert.spv"), fs = load_spv(&v, "tri.frag.spv");
   VkPipelineShaderStageCreateInfo st[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
   VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, W, H, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { W, H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba };
   VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
      .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0 };
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(v.dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = v.qfam };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(v.dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(v.dev, &cai, &cmd));
   VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(v.dev, &fi, NULL, &fence));
   for (unsigned n = 0;; n++) {
      CHECK(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      VkClearValue cv = { .color = { { (n % 100) / 100.0f, 0.2f, 0.5f, 1.0f } } };
      VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
         .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 1, .pClearValues = &cv };
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      for (int i = 0; i < 4; i++) vkCmdDraw(cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(cmd);
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      CHECK(vkQueueSubmit(v.q, 1, &si, fence));
      CHECK(vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 10000000000ull));
      CHECK(vkResetFences(v.dev, 1, &fence));
      usleep(frame_sleep_us);
      (void)idx;
   }
}

/* ------------------------------------------------------------------ main test */
static void usage(const char *a0)
{
   fprintf(stderr,
      "usage: %s [options]\n"
      "  --scenario yt|shadow|small|both|yt-b|shadow-b|small-b|both-b   built-in chains (default both;\n"
      "                       the -b ones draw + barrier inside the pass; append -s (e.g. yt-s, both-s) to run every\n"
      "                       pass from a SECONDARY command buffer with a draw, -e for an empty secondary: this is what\n"
      "                       DXVK does on tilers and what the guest's hanging jobs really are, see README.md)\n"
      "  --shapes LIST        custom chain, e.g. '128x16#5!s,120x8@128x16#6!s,120x8@128x16#5!s'\n"
      "                       FWxFH[@IWxIH][#slot][!c clear][!y copy-before][!d draw][!b draw+in-pass barrier]\n"
      "                       [!s secondary cmdbuf with a draw][!e empty secondary cmdbuf]\n"
      "                       [!t secondary with a textured draw: samples the previous pass's image, vertex buffer + UBO; suffix -t]\n"
      "  --iters N            iterations (default 4000)\n"
      "  --delay-us N         sleep between iterations (default 4000, ~ a DWM frame)\n"
      "  --one-submit-per-pass  submit every pass separately instead of one command buffer\n"
      "  --no-barriers        no pipeline barriers between passes\n"
      "  --bg N               start N background drawing processes first (compositor-like load)\n"
      "  --bg-sleep-us N      pause between background frames (default 3000)\n"
      "  --cpu N              start N busy-loop processes (the VM keeps 3 vCPUs at 100%%)\n"
      "  --procs N            run the test chain in N processes at once (the guest has several\n"
      "                       render-server contexts submitting concurrently)\n"
      "  --heap-mb N          size of the shared image heap (default 32)\n"
      "  --slow-ms N          report submits slower than this (default 300)\n"
      "  --spv-dir DIR        where tri.vert.spv / tri.frag.spv are (default: next to the binary)\n"
      "  -v                   verbose\n", a0);
}

int main(int argc, char **argv)
{
   const char *scen = "both", *custom = NULL;
   unsigned iters = 4000, delay_us = 4000, nbg = 0, bg_sleep = 3000, heap_mb = 32, slow_ms = 300;
   unsigned cpu_spin = 0, procs = 1;
   bool is_peer = false;
   bool per_pass = false, no_barriers = false, verbose = false;
   char exe[1024];
   ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
   if (el > 0) { exe[el] = 0; char *s = strrchr(exe, '/'); if (s) *s = 0; } else strcpy(exe, ".");
   spv_dir = exe;
   for (int i = 1; i < argc; i++) {
      const char *a = argv[i];
      #define ARG(name) (!strcmp(a, name) && i + 1 < argc)
      if (ARG("--scenario")) scen = argv[++i];
      else if (ARG("--shapes")) custom = argv[++i];
      else if (ARG("--iters")) iters = atoi(argv[++i]);
      else if (ARG("--delay-us")) delay_us = atoi(argv[++i]);
      else if (ARG("--bg")) nbg = atoi(argv[++i]);
      else if (ARG("--bg-sleep-us")) bg_sleep = atoi(argv[++i]);
      else if (ARG("--cpu")) cpu_spin = atoi(argv[++i]);
      else if (ARG("--procs")) procs = atoi(argv[++i]);
      else if (ARG("--heap-mb")) heap_mb = atoi(argv[++i]);
      else if (ARG("--slow-ms")) slow_ms = atoi(argv[++i]);
      else if (ARG("--spv-dir")) spv_dir = argv[++i];
      else if (!strcmp(a, "--one-submit-per-pass")) per_pass = true;
      else if (!strcmp(a, "--no-barriers")) no_barriers = true;
      else if (!strcmp(a, "-v")) verbose = true;
      else { usage(argv[0]); return 2; }
   }
   char both[1024], sfxbuf[2048], base[64];
   const char *chain, *sfx = NULL;
   size_t sl = strlen(scen);
   /* "NAME-s": every pass of NAME takes its contents from a secondary command buffer with one draw (as
    * DXVK does on tilers); "NAME-e": secondary command buffer with no commands */
   if (!custom && sl > 2 && scen[sl - 2] == '-' && (scen[sl - 1] == 's' || scen[sl - 1] == 'e' || scen[sl - 1] == 't')) {
      sfx = scen[sl - 1] == 's' ? "!s" : scen[sl - 1] == 'e' ? "!e" : "!t";
      snprintf(base, sizeof base, "%.*s", (int)(sl - 2), scen);
      scen = base;
   }
   if (custom) chain = custom;
   else if (!strcmp(scen, "yt")) chain = SCEN_YT;
   else if (!strcmp(scen, "shadow")) chain = SCEN_SHADOW;
   else if (!strcmp(scen, "small")) chain = SCEN_SMALL;
   else if (!strcmp(scen, "yt-b")) chain = SCEN_YT_B;
   else if (!strcmp(scen, "shadow-b")) chain = SCEN_SHADOW_B;
   else if (!strcmp(scen, "small-b")) chain = SCEN_SMALL_B;
   else if (!strcmp(scen, "both-b")) { snprintf(both, sizeof both, "%s,%s", SCEN_YT_B, SCEN_SHADOW_B); chain = both; }
   else { snprintf(both, sizeof both, "%s,%s", SCEN_YT, SCEN_SHADOW); chain = both; }
   if (sfx) {
      size_t o = 0;
      for (const char *p = chain; *p && o + 8 < sizeof sfxbuf; p++) {
         if (*p == ',') { o += snprintf(sfxbuf + o, sizeof sfxbuf - o, "%s", sfx); }
         sfxbuf[o++] = *p;
      }
      o += snprintf(sfxbuf + o, sizeof sfxbuf - o, "%s", sfx);
      sfxbuf[o] = 0;
      chain = sfxbuf;
   }
   if (parse_shapes(chain)) { fprintf(stderr, "bad shape list: %s\n", chain); return 2; }

   pid_t bgpids[16];
   if (nbg > 16) nbg = 16;
   for (unsigned i = 0; i < nbg; i++) {
      pid_t p = fork();
      if (p == 0) { bg_main(i, bg_sleep); _exit(0); }
      bgpids[i] = p;
   }
   pid_t cpupids[16];
   if (cpu_spin > 16) cpu_spin = 16;
   for (unsigned i = 0; i < cpu_spin; i++) {
      pid_t p = fork();
      if (p == 0) { for (;;) __asm__ volatile("" ::: "memory"); }
      cpupids[i] = p;
   }
   if (nbg) sleep(2);
   pid_t peers[16];
   if (procs > 16) procs = 16;
   for (unsigned i = 1; i < procs; i++) {
      pid_t p = fork();
      if (p == 0) { is_peer = true; nbg = 0; cpu_spin = 0; break; }
      peers[i] = p;
   }

   struct vk v = { 0 };
   vk_init(&v, verbose);
   printf("device: %s\nchain: %s\niters %u, delay %u us, bg %u, %s, %s\n", v.name, chain, iters, delay_us, nbg,
          per_pass ? "one submit per pass" : "one submit per iteration", no_barriers ? "no barriers" : "barriers");

   /* shared heap for the small images (like DXVK's 32 MB chunk) */
   heap_size = (VkDeviceSize)heap_mb << 20;
   {
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
         .extent = { 16, 16, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
      VkImage probe;
      CHECK(vkCreateImage(v.dev, &ici, NULL, &probe));
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(v.dev, probe, &mr);
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = heap_size,
         .memoryTypeIndex = find_mem(&v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      CHECK(vkAllocateMemory(v.dev, &mai, NULL, &heap));
      vkDestroyImage(v.dev, probe, NULL);
   }

   VkRenderPass rp_load = make_rp(&v, false), rp_clear = make_rp(&v, true);
   VkPipeline tri_pipe = make_tri_pipeline(&v, rp_load);   /* also valid with rp_clear (compatible) */
   VkFramebuffer fbs[MAX_SHAPES];
   struct img *simg[MAX_SHAPES];
   for (int i = 0; i < nshapes; i++) {
      simg[i] = get_img(&v, &shapes[i]);
      VkFramebufferCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .renderPass = shapes[i].clear ? rp_clear : rp_load, .attachmentCount = 1, .pAttachments = &simg[i]->view,
         .width = shapes[i].fw, .height = shapes[i].fh, .layers = 1 };
      CHECK(vkCreateFramebuffer(v.dev, &fci, NULL, &fbs[i]));
   }

   /* textured shapes (!t): each samples the previous pass's image (different slot) with a vertex buffer and a UBO */
   int texsrc[MAX_SHAPES];
   VkPipeline tex_pipe = VK_NULL_HANDLE;
   VkDescriptorSet dsets[MAX_SHAPES];
   VkBuffer vbuf = VK_NULL_HANDLE;
   {
      bool any_tex = false;
      for (int i = 0; i < nshapes; i++) {
         texsrc[i] = -1;
         if (!shapes[i].tex) continue;
         for (int k = 1; k <= nshapes && texsrc[i] < 0; k++) {
            int j = ((i - k) % nshapes + nshapes) % nshapes;
            if (shapes[j].slot != shapes[i].slot) texsrc[i] = j;
         }
         if (texsrc[i] >= 0) any_tex = true;
      }
      if (any_tex) {
         tex_pipe = make_tex_pipeline(&v, rp_load);
         VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096,
            .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
         CHECK(vkCreateBuffer(v.dev, &bci, NULL, &vbuf));
         VkMemoryRequirements bmr;
         vkGetBufferMemoryRequirements(v.dev, vbuf, &bmr);
         VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
            .memoryTypeIndex = find_mem(&v, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
         VkDeviceMemory bmem;
         CHECK(vkAllocateMemory(v.dev, &bai, NULL, &bmem));
         CHECK(vkBindBufferMemory(v.dev, vbuf, bmem, 0));
         float *mp;
         CHECK(vkMapMemory(v.dev, bmem, 0, 4096, 0, (void **)&mp));
         static const float verts[12] = { -1, -1, 0, 0,   3, -1, 2, 0,   -1, 3, 0, 2 };
         memcpy(mp, verts, sizeof verts);
         float tint[4] = { 1, 1, 1, 1 };
         memcpy((char *)mp + 256, tint, sizeof tint);
         VkSamplerCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxLod = 1.0f };
         VkSampler smp;
         CHECK(vkCreateSampler(v.dev, &smi, NULL, &smp));
         VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)nshapes },
                                        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)nshapes } };
         VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = (uint32_t)nshapes, .poolSizeCount = 2, .pPoolSizes = ps };
         VkDescriptorPool dpool;
         CHECK(vkCreateDescriptorPool(v.dev, &dpi, NULL, &dpool));
         for (int i = 0; i < nshapes; i++) {
            if (texsrc[i] < 0) continue;
            VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
               .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &tex_dsl };
            CHECK(vkAllocateDescriptorSets(v.dev, &dai, &dsets[i]));
            VkDescriptorImageInfo dii = { .sampler = smp, .imageView = simg[texsrc[i]]->view,
               .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorBufferInfo dbi = { .buffer = vbuf, .offset = 256, .range = 16 };
            VkWriteDescriptorSet w[2] = {
               { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[i], .dstBinding = 0, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii },
               { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dsets[i], .dstBinding = 1, .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dbi } };
            vkUpdateDescriptorSets(v.dev, 2, w, 0, NULL);
         }
      }
   }

   /* scratch copy source (largest image size) */
   uint32_t sw = 128, sh_ = 128;
   {
      VkImageCreateInfo ici = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
         .extent = { sw, sh_, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
         .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
      CHECK(vkCreateImage(v.dev, &ici, NULL, &scratch));
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(v.dev, scratch, &mr);
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
         .memoryTypeIndex = find_mem(&v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
      VkDeviceMemory m;
      CHECK(vkAllocateMemory(v.dev, &mai, NULL, &m));
      CHECK(vkBindImageMemory(v.dev, scratch, m, 0));
   }

   VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = v.qfam };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(v.dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(v.dev, &cai, &cmd));
   /* secondary command buffers (one per shape, re-recorded every iteration like DXVK does) */
   VkCommandPool spool;
   CHECK(vkCreateCommandPool(v.dev, &cpi, NULL, &spool));
   VkCommandBuffer seccb[MAX_SHAPES];
   {
      VkCommandBufferAllocateInfo sai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = spool,
         .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = (uint32_t)nshapes };
      CHECK(vkAllocateCommandBuffers(v.dev, &sai, seccb));
   }
   VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(v.dev, &fi, NULL, &fence));

   /* one-time init: clear every image to a color and move it to COLOR_ATTACHMENT_OPTIMAL */
   {
      VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
      CHECK(vkBeginCommandBuffer(cmd, &bi));
      image_barrier(cmd, scratch, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue cc = { .float32 = { 0.1f, 0.4f, 0.8f, 1.0f } };
      VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cmd, scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
      image_barrier(cmd, scratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      for (int i = 0; i < nimgs; i++) {
         image_barrier(cmd, imgs[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         VkClearColorValue c2 = { .float32 = { 0.9f, 0.3f, 0.1f, 1.0f } };
         vkCmdClearColorImage(cmd, imgs[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c2, 1, &rng);
         image_barrier(cmd, imgs[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      }
      CHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
      CHECK(vkQueueSubmit(v.q, 1, &si, fence));
      CHECK(vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 10000000000ull));
      CHECK(vkResetFences(v.dev, 1, &fence));
   }

   unsigned slow = 0;
   double worst = 0, total = 0;
   const double t_start = now_ms();
   for (unsigned it = 0; it < iters; it++) {
      double t0 = now_ms();
      int first = 0;
      while (first < nshapes) {
         int last = per_pass ? first + 1 : nshapes;
         CHECK(vkResetCommandBuffer(cmd, 0));
         VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
         CHECK(vkBeginCommandBuffer(cmd, &bi));
         for (int i = first; i < last; i++) {
            const struct shape *sh = &shapes[i];
            if (sh->copy_before) {
               image_barrier(cmd, simg[i]->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
               VkImageCopy rg = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .extent = { sh->iw < sw ? sh->iw : sw, sh->ih < sh_ ? sh->ih : sh_, 1 } };
               vkCmdCopyImage(cmd, scratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, simg[i]->image,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
               image_barrier(cmd, simg[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            } else if (!no_barriers) {
               barrier_all(cmd);
            }
            VkClearValue cv = { .color = { { 0.0f, (it % 16) / 16.0f, 0.5f, 1.0f } } };
            VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
               .renderPass = sh->clear ? rp_clear : rp_load, .framebuffer = fbs[i],
               .renderArea = { { 0, 0 }, { sh->fw, sh->fh } }, .clearValueCount = 1, .pClearValues = &cv };
            if (sh->sec || sh->sec_empty) {
               VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
                  .renderPass = sh->clear ? rp_clear : rp_load, .subpass = 0, .framebuffer = fbs[i] };
               VkCommandBufferBeginInfo sbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                  .flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                  .pInheritanceInfo = &inh };
               CHECK(vkResetCommandBuffer(seccb[i], 0));
               CHECK(vkBeginCommandBuffer(seccb[i], &sbi));
               if (sh->sec) {
                  VkViewport vp = { 0, 0, (float)sh->fw, (float)sh->fh, 0.0f, 1.0f };
                  VkRect2D sc = { { 0, 0 }, { sh->fw, sh->fh } };
                  if (texsrc[i] >= 0) {
                     VkDeviceSize voff = 0;
                     vkCmdBindPipeline(seccb[i], VK_PIPELINE_BIND_POINT_GRAPHICS, tex_pipe);
                     vkCmdBindDescriptorSets(seccb[i], VK_PIPELINE_BIND_POINT_GRAPHICS, tex_pl, 0, 1, &dsets[i], 0, NULL);
                     vkCmdBindVertexBuffers(seccb[i], 0, 1, &vbuf, &voff);
                  } else
                  vkCmdBindPipeline(seccb[i], VK_PIPELINE_BIND_POINT_GRAPHICS, tri_pipe);
                  vkCmdSetViewport(seccb[i], 0, 1, &vp);
                  vkCmdSetScissor(seccb[i], 0, 1, &sc);
                  vkCmdDraw(seccb[i], 3, 1, 0, 0);
               }
               CHECK(vkEndCommandBuffer(seccb[i]));
               vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
               vkCmdExecuteCommands(cmd, 1, &seccb[i]);
               vkCmdEndRenderPass(cmd);
               continue;
            }
            vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
            if (sh->draw || sh->split) {
               VkViewport vp = { 0, 0, (float)sh->fw, (float)sh->fh, 0.0f, 1.0f };
               VkRect2D sc = { { 0, 0 }, { sh->fw, sh->fh } };
               vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tri_pipe);
               vkCmdSetViewport(cmd, 0, 1, &vp);
               vkCmdSetScissor(cmd, 0, 1, &sc);
               vkCmdDraw(cmd, 3, 1, 0, 0);
               if (sh->split) inpass_barrier(cmd);
            }
            vkCmdEndRenderPass(cmd);
         }
         CHECK(vkEndCommandBuffer(cmd));
         VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
         CHECK(vkQueueSubmit(v.q, 1, &si, fence));
         VkResult r = vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 8000000000ull);
         if (r != VK_SUCCESS) {
            fprintf(stderr, "iteration %u: fence wait returned %d (device lost?)\n", it, r);
            goto done;
         }
         CHECK(vkResetFences(v.dev, 1, &fence));
         first = last;
      }
      double dt = now_ms() - t0;
      total += dt;
      if (dt > worst) worst = dt;
      if (dt > slow_ms) {
         slow++;
         printf("SLOW iteration %u: %.0f ms  (t=%.3f s)\n", it, dt, (now_ms() - t_start) / 1000.0);
         fflush(stdout);
      }
      if (verbose && it % 500 == 0) printf("iter %u avg %.2f ms\n", it, total / (it + 1));
      usleep(delay_us);
   }
done:
   printf("done: %u slow iterations (> %u ms), worst %.1f ms, avg %.2f ms\n", slow, slow_ms, worst, total / (iters ? iters : 1));
   printf("now compare: sudo dmesg | grep -c 'Resetting GPU'\n");
   if (is_peer) return 0;
   for (unsigned i = 1; i < procs; i++) waitpid(peers[i], NULL, 0);
   for (unsigned i = 0; i < nbg; i++) { kill(bgpids[i], SIGTERM); waitpid(bgpids[i], NULL, 0); }
   for (unsigned i = 0; i < cpu_spin; i++) { kill(cpupids[i], SIGTERM); waitpid(cpupids[i], NULL, 0); }
   return 0;
}

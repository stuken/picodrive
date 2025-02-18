/*
 * libretro core glue for PicoDrive
 * (C) notaz, 2013
 * (C) aliaspider, 2016
 * (C) Daniel De Matteis, 2013
 * (C) kub, 2020
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1 // mremap
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#ifndef _WIN32
#ifndef NO_MMAP
#ifdef __SWITCH__
#include "switch/mman.h"
#else
#include <sys/mman.h>
#endif
#endif
#else
#include <io.h>
#include <windows.h>
#include <sys/types.h>
#endif
#include <errno.h>
#ifdef __MACH__
#include <libkern/OSCacheControl.h>
#endif

#ifdef USE_LIBRETRO_VFS
#include "file_stream_transforms.h"
#endif

#if defined(RENDER_GSKIT_PS2)
#include <malloc.h>
#include "libretro-common/include/libretro_gskit_ps2.h"
#include "ps2/asm.h"
#endif

#ifdef _3DS
#include "3ds/3ds_utils.h"
#define MEMOP_MAP     4
#define MEMOP_UNMAP   5
#define MEMOP_PROT    6

int svcDuplicateHandle(unsigned int* out, unsigned int original);
int svcCloseHandle(unsigned int handle);
int svcControlProcessMemory(unsigned int process, void* addr0, void* addr1,
                            unsigned int size, unsigned int type, unsigned int perm);
void* linearMemAlign(size_t size, size_t alignment);
void linearFree(void* mem);

static int ctr_svchack_successful = 0;

#elif defined(VITA)
#define TARGET_SIZE_2 24 // 2^24 = 16 megabytes

#include <psp2/kernel/sysmem.h>
static int sceBlock;
int getVMBlock();
int _newlib_vm_size_user = 1 << TARGET_SIZE_2;
#endif

#include "libretro_core_options.h"

#include <pico/pico_int.h>
#include <pico/state.h>
#include <pico/patch.h>
#include <pico/sound/mix.h>
#include "../common/input_pico.h"
#include "../common/version.h"
#include <libretro.h>
#include <compat/strcasestr.h>

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;
static retro_audio_sample_batch_t audio_batch_cb;

#define VOUT_MAX_WIDTH 320
#define VOUT_32COL_WIDTH 256
#define VOUT_MAX_HEIGHT 240

#define INITIAL_SND_RATE 44100

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const float VOUT_PAR = 0.0;
static const float VOUT_4_3 = (224.0f * (4.0f / 3.0f));
static const float VOUT_CRT = (224.0f * 1.29911f);

static bool show_overscan = false;
static bool old_show_overscan = false;

/* Required to allow on the fly changes to 'show overscan' */
static int vm_current_start_line = -1;
static int vm_current_line_count = -1;
static int vm_current_is_32cols = -1;

static int vout_16bit = 1;
static int vout_format = PDF_RGB555;
static void *vout_buf;
static int vout_width, vout_height, vout_offset;
static float user_vout_width = 0.0;

#if defined(RENDER_GSKIT_PS2)
#define VOUT_8BIT_WIDTH 328
#define VOUT_8BIT_HEIGHT 256
RETRO_HW_RENDER_INTEFACE_GSKIT_PS2 *ps2 = NULL;
static void *retro_palette;
static struct retro_hw_ps2_insets padding;
#endif

static short ALIGNED(4) sndBuffer[2*INITIAL_SND_RATE/50];

static void snd_write(int len);

char **g_argv;

#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

/* Frameskipping Support */

static unsigned frameskip_type             = 0;
static unsigned frameskip_threshold        = 0;
static uint16_t frameskip_counter          = 0;

static bool retro_audio_buff_active        = false;
static unsigned retro_audio_buff_occupancy = 0;
static bool retro_audio_buff_underrun      = false;
/* Maximum number of consecutive frames that
 * can be skipped */
#define FRAMESKIP_MAX 60

static unsigned audio_latency              = 0;
static bool update_audio_latency           = false;

static void retro_audio_buff_status_cb(
      bool active, unsigned occupancy, bool underrun_likely)
{
   retro_audio_buff_active    = active;
   retro_audio_buff_occupancy = occupancy;
   retro_audio_buff_underrun  = underrun_likely;
}

static void init_frameskip(void)
{
   if (frameskip_type > 0)
   {
      struct retro_audio_buffer_status_callback buf_status_cb;

      buf_status_cb.callback = retro_audio_buff_status_cb;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK,
            &buf_status_cb))
      {
         if (log_cb)
            log_cb(RETRO_LOG_WARN, "Frameskip disabled - frontend does not support audio buffer status monitoring.\n");

         retro_audio_buff_active    = false;
         retro_audio_buff_occupancy = 0;
         retro_audio_buff_underrun  = false;
         audio_latency              = 0;
      }
      else
      {
         /* Frameskip is enabled - increase frontend
          * audio latency to minimise potential
          * buffer underruns */
         float frame_time_msec = 1000.0f / (Pico.m.pal ? 50.0f : 60.0f);

         /* Set latency to 6x current frame time... */
         audio_latency = (unsigned)((6.0f * frame_time_msec) + 0.5f);

         /* ...then round up to nearest multiple of 32 */
         audio_latency = (audio_latency + 0x1F) & ~0x1F;
      }
   }
   else
   {
      environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, NULL);
      audio_latency = 0;
   }

   update_audio_latency = true;
}

/* functions called by the core */

void cache_flush_d_inval_i(void *start, void *end)
{
#ifdef __arm__
   size_t len = (char *)end - (char *)start;
   (void)len;
#if defined(__BLACKBERRY_QNX__)
   msync(start, end - start, MS_SYNC | MS_CACHE_ONLY | MS_INVALIDATE_ICACHE);
#elif defined(__MACH__)
   sys_dcache_flush(start, len);
   sys_icache_invalidate(start, len);
#elif defined(_3DS)
   ctr_flush_invalidate_cache();
#elif defined(VITA)
   sceKernelSyncVMDomain(sceBlock, start, len);
#else
   __clear_cache(start, end);
#endif
#endif
}

#ifdef RENDER_GSKIT_PS2
/* In PS2 toolchain these aren't yet defined */
void _flush_cache(void *b, void *e)
{
#if 0 /* which of these is overall faster for lots of small cache updates? */
   SyncDCache(b, e);
#else
   FlushCache(0); /* WRITEBACK_DCACHE */
#endif
   FlushCache(2); /* INVALIDATE_ICACHE */
}

int __builtin_parity(unsigned v)
{
   /* credits to bit twiddling hacks, https://graphics.stanford.edu/~seander/bithacks.html */
   v ^= v >> 16;
   v ^= v >> 8;
   v ^= v >> 4;
   return (0x6996 >> (v&0xf)) & 1;
}
#elif defined(PSP)
int _flush_cache(char *addr, const int size, const int op)
{
   //sceKernelDcacheWritebackAll();
   sceKernelDcacheWritebackRange(addr, size);
   sceKernelIcacheInvalidateRange(addr, size);
   return 0;
}
#endif

#ifdef __MACH__
/* calls to this may be generated by the compiler, but it's missing in libc? */
void __clear_cache(void *start, void *end)
{
   size_t len = (char *)end - (char *)start;
   sys_dcache_flush(start, len);
   sys_icache_invalidate(start, len);
}
#endif

#ifdef _WIN32
/* mmap() replacement for Windows
 *
 * Author: Mike Frysinger <vapier@gentoo.org>
 * Placed into the public domain
 */

/* References:
 * CreateFileMapping: http://msdn.microsoft.com/en-us/library/aa366537(VS.85).aspx
 * CloseHandle:       http://msdn.microsoft.com/en-us/library/ms724211(VS.85).aspx
 * MapViewOfFile:     http://msdn.microsoft.com/en-us/library/aa366761(VS.85).aspx
 * UnmapViewOfFile:   http://msdn.microsoft.com/en-us/library/aa366882(VS.85).aspx
 */

#define PROT_READ     0x1
#define PROT_WRITE    0x2
/* This flag is only available in WinXP+ */
#ifdef FILE_MAP_EXECUTE
#define PROT_EXEC     0x4
#else
#define PROT_EXEC        0x0
#define FILE_MAP_EXECUTE 0
#endif

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *) -1)

#ifdef __USE_FILE_OFFSET64
# define DWORD_HI(x) (x >> 32)
# define DWORD_LO(x) ((x) & 0xffffffff)
#else
# define DWORD_HI(x) (0)
# define DWORD_LO(x) (x)
#endif

static void *mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
   uint32_t flProtect, dwDesiredAccess;
   off_t end;
   HANDLE mmap_fd, h;
   void *ret;

   if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
      return MAP_FAILED;
   if (fd == -1) {
      if (!(flags & MAP_ANON) || offset)
         return MAP_FAILED;
   } else if (flags & MAP_ANON)
      return MAP_FAILED;

   if (prot & PROT_WRITE) {
      if (prot & PROT_EXEC)
         flProtect = PAGE_EXECUTE_READWRITE;
      else
         flProtect = PAGE_READWRITE;
   } else if (prot & PROT_EXEC) {
      if (prot & PROT_READ)
         flProtect = PAGE_EXECUTE_READ;
      else if (prot & PROT_EXEC)
         flProtect = PAGE_EXECUTE;
   } else
      flProtect = PAGE_READONLY;

   end = length + offset;

   if (fd == -1)
      mmap_fd = INVALID_HANDLE_VALUE;
   else
      mmap_fd = (HANDLE)_get_osfhandle(fd);
   h = CreateFileMapping(mmap_fd, NULL, flProtect, DWORD_HI(end), DWORD_LO(end), NULL);
   if (h == NULL)
      return MAP_FAILED;

   if (prot & PROT_WRITE)
      dwDesiredAccess = FILE_MAP_WRITE;
   else
      dwDesiredAccess = FILE_MAP_READ;
   if (prot & PROT_EXEC)
      dwDesiredAccess |= FILE_MAP_EXECUTE;
   if (flags & MAP_PRIVATE)
      dwDesiredAccess |= FILE_MAP_COPY;
   ret = MapViewOfFile(h, dwDesiredAccess, DWORD_HI(offset), DWORD_LO(offset), length);
   if (ret == NULL) {
      CloseHandle(h);
      ret = MAP_FAILED;
   }
   return ret;
}

static void munmap(void *addr, size_t length)
{
   UnmapViewOfFile(addr);
   /* ruh-ro, we leaked handle from CreateFileMapping() ... */
}
#elif defined(NO_MMAP)
#define PROT_EXEC   0x04
#define MAP_FAILED 0
#define PROT_READ 0
#define PROT_WRITE 0
#define MAP_PRIVATE 0
#define MAP_ANONYMOUS 0

void* mmap(void *desired_addr, size_t len, int mmap_prot, int mmap_flags, int fildes, size_t off)
{
   return calloc(1, len);
}

void munmap(void *base_addr, size_t len)
{
   free(base_addr);
}

int mprotect(void *addr, size_t len, int prot)
{
   /* stub - not really needed at this point since this codepath has no dynarecs */
   return 0;
}

#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifdef _3DS
typedef struct
{
   unsigned int requested_map;
   void* buffer;
}pico_mmap_t;

pico_mmap_t pico_mmaps[] = {
   {0x02000000, 0},
   {0x06000000, 0},
   {NULL,       0}
};

void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
   (void)is_fixed;

   if (ctr_svchack_successful)
   {
      pico_mmap_t* pico_mmap;

      for (pico_mmap = pico_mmaps; pico_mmap->requested_map; pico_mmap++)
      {
         if ((pico_mmap->requested_map == addr))
         {
            unsigned int ptr_aligned, tmp;
            unsigned int currentHandle;
            unsigned int perm = 0b011;

            if (need_exec)
               perm = 0b111;

            size = (size + 0xFFF) & ~0xFFF;
            pico_mmap->buffer = malloc(size + 0x1000);
            ptr_aligned = (((unsigned int)pico_mmap->buffer) + 0xFFF) & ~0xFFF;

            svcDuplicateHandle(&currentHandle, 0xFFFF8001);

            if(svcControlProcessMemory(currentHandle, pico_mmap->requested_map, ptr_aligned, size, MEMOP_MAP, perm) < 0)
            {
               if (log_cb)
                  log_cb(RETRO_LOG_ERROR, "could not map memory @0x%08X\n", pico_mmap->requested_map);
               exit(1);
            }

            svcCloseHandle(currentHandle);
            return (void*)pico_mmap->requested_map;
         }
      }
   }

   return malloc(size);
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
   if (ctr_svchack_successful)
   {
      pico_mmap_t* pico_mmap;

      for (pico_mmap = pico_mmaps; pico_mmap->requested_map; pico_mmap++)
      {
         if ((pico_mmap->requested_map == (unsigned int)ptr))
         {
            unsigned int ptr_aligned;
            unsigned int currentHandle;
            void* tmp;

            oldsize = (oldsize + 0xFFF) & ~0xFFF;
            newsize = (newsize + 0xFFF) & ~0xFFF;
            ptr_aligned = (((unsigned int)pico_mmap->buffer) + 0xFFF) & ~0xFFF;

            svcDuplicateHandle(&currentHandle, 0xFFFF8001);

            svcControlProcessMemory(currentHandle, pico_mmap->requested_map, ptr_aligned, oldsize, MEMOP_UNMAP, 0b011);

            tmp = realloc(pico_mmap->buffer, newsize + 0x1000);
            if(!tmp)
               return NULL;

            pico_mmap->buffer = tmp;
            ptr_aligned = (((unsigned int)pico_mmap->buffer) + 0xFFF) & ~0xFFF;

            svcControlProcessMemory(currentHandle, pico_mmap->requested_map, ptr_aligned, newsize, MEMOP_MAP, 0x3);

            svcCloseHandle(currentHandle);

            return ptr;
         }
      }
   }

   return realloc(ptr, newsize);

}
void plat_munmap(void *ptr, size_t size)
{
   if (ctr_svchack_successful)
   {
      pico_mmap_t* pico_mmap;

      for (pico_mmap = pico_mmaps; pico_mmap->requested_map; pico_mmap++)
      {
         if ((pico_mmap->requested_map == (unsigned int)ptr))
         {
            unsigned int ptr_aligned;
            unsigned int currentHandle;

            size = (size + 0xFFF) & ~0xFFF;
            ptr_aligned = (((unsigned int)pico_mmap->buffer) + 0xFFF) & ~0xFFF;

            svcDuplicateHandle(&currentHandle, 0xFFFF8001);

            svcControlProcessMemory(currentHandle, (void*)pico_mmap->requested_map, (void*)ptr_aligned, size, MEMOP_UNMAP, 0b011);

            svcCloseHandle(currentHandle);

            free(pico_mmap->buffer);
            pico_mmap->buffer = NULL;
            return;
         }
      }
   }

   free(ptr);
}

#else
void *plat_mmap(unsigned long addr, size_t size, int need_exec, int is_fixed)
{
   int flags = MAP_PRIVATE | MAP_ANONYMOUS;
   void *req, *ret;

   req = (void *)(uintptr_t)addr;
   ret = mmap(req, size, PROT_READ | PROT_WRITE, flags, -1, 0);
   if (ret == MAP_FAILED) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "mmap(%08lx, %zd) failed: %d\n", addr, size, errno);
      return NULL;
   }

   if (addr != 0 && ret != (void *)(uintptr_t)addr) {
      if (log_cb)
         log_cb(RETRO_LOG_WARN, "warning: wanted to map @%08lx, got %p\n",
               addr, ret);

      if (is_fixed) {
         munmap(ret, size);
         return NULL;
      }
   }

   return ret;
}

void *plat_mremap(void *ptr, size_t oldsize, size_t newsize)
{
#if defined(__linux__) && !defined(__SWITCH__)
   void *ret = mremap(ptr, oldsize, newsize, 0);
   if (ret == MAP_FAILED)
      return NULL;

   return ret;
#else
   void *tmp, *ret;
   size_t preserve_size;

   preserve_size = oldsize;
   if (preserve_size > newsize)
      preserve_size = newsize;
   tmp = malloc(preserve_size);
   if (tmp == NULL)
      return NULL;
   memcpy(tmp, ptr, preserve_size);

   munmap(ptr, oldsize);
   ret = mmap(ptr, newsize, PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (ret == MAP_FAILED) {
      free(tmp);
      return NULL;
   }
   memcpy(ret, tmp, preserve_size);
   free(tmp);
   return ret;
#endif
}

void plat_munmap(void *ptr, size_t size)
{
   if (ptr != NULL)
      munmap(ptr, size);
}
#endif

// if NULL is returned, static buffer is used
void *plat_mem_get_for_drc(size_t size)
{
   void *mem = NULL;
#ifdef VITA
   sceKernelGetMemBlockBase(sceBlock, &mem);
#endif
   return mem;
}

int plat_mem_set_exec(void *ptr, size_t size)
{
   int ret = -1;
#ifdef _WIN32
   DWORD oldProtect = 0;
   ret = VirtualProtect(ptr, size, PAGE_EXECUTE_READWRITE, &oldProtect);
   if (ret == 0 && log_cb)
      log_cb(RETRO_LOG_ERROR, "VirtualProtect(%p, %d) failed: %d\n", ptr, (int)size,
             GetLastError());
#elif defined(_3DS)
   if (ctr_svchack_successful)
   {
      unsigned int currentHandle;
      svcDuplicateHandle(&currentHandle, 0xFFFF8001);
      ret = svcControlProcessMemory(currentHandle, ptr, 0x0,
                              size, MEMOP_PROT, 0b111);
      svcCloseHandle(currentHandle);
      ctr_flush_invalidate_cache();

   }
   else
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "plat_mem_set_exec called with no svcControlProcessMemory access\n");
      exit(1);
   }

#elif defined(VITA)
   ret = sceKernelOpenVMDomain();
#else
   ret = mprotect(ptr, size, PROT_READ | PROT_WRITE | PROT_EXEC);
   if (ret != 0 && log_cb)
      log_cb(RETRO_LOG_ERROR, "mprotect(%p, %zd) failed: %d\n", ptr, size, errno);
#endif
   return ret;
}

void emu_video_mode_change(int start_line, int line_count, int is_32cols)
{
   struct retro_system_av_info av_info;

   vm_current_start_line = start_line;
   vm_current_line_count = line_count;
   vm_current_is_32cols = is_32cols;

   // 8bit renderes create a 328x256 CLUT image, while 16bit creates 320x240 RGB
   vout_16bit = vout_format == PDF_RGB555 || (PicoIn.AHW & PAHW_32X);

#if defined(RENDER_GSKIT_PS2)
   // calculate the borders of the real image inside the picodrive image
   vout_width = (vout_16bit ? VOUT_MAX_WIDTH : VOUT_8BIT_WIDTH);
   vout_height = (vout_16bit ? VOUT_MAX_HEIGHT : VOUT_8BIT_HEIGHT);
   vout_offset = (vout_16bit ? 0 : 8); // 8bit has 8 px overlap area on the left
   if (is_32cols) {
      // 256x240, with or w/o overlap on the left and 64 px on the right
      padding = (struct retro_hw_ps2_insets){start_line, vout_offset, vout_height - line_count - start_line, vout_width - 256.0f - vout_offset};
   } else {
      // 320x240, with or w/o overlap on the left and none on the right
      padding = (struct retro_hw_ps2_insets){start_line, vout_offset, vout_height - line_count - start_line, vout_width - 320.0f - vout_offset};
   }

   int pxsz = (vout_16bit ? 2 : 1); // pixel size: RGB = 16 bits, CLUT = 8 bits
   memset(vout_buf, 0, pxsz * vout_width * vout_height);
   memset(retro_palette, 0, gsKit_texture_size_ee(16, 16, GS_PSM_CT16));
   PicoDrawSetOutBuf(vout_buf, pxsz * vout_width);
   if (ps2) {
      // prepare image as texture for rendering
      ps2->coreTexture->Width = vout_width;
      ps2->coreTexture->Height = vout_height;
      ps2->coreTexture->PSM = (vout_16bit ? GS_PSM_CT16 : GS_PSM_T8);
      ps2->padding = padding;
   }
#else
   vout_width = is_32cols ? VOUT_32COL_WIDTH : VOUT_MAX_WIDTH;
   memset(vout_buf, 0, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);  
   if (vout_16bit)
      PicoDrawSetOutBuf(vout_buf, vout_width * 2);

   if (show_overscan)
   {
      vout_height = line_count + (start_line * 2);
      vout_offset = 0;
   }
   else
   {
      vout_height = line_count;
      /* Note: We multiply by 2 here to account for pitch */
      vout_offset = vout_width * start_line * 2;
   }

   /* Redundant sanity check... */
   vout_height = (vout_height > VOUT_MAX_HEIGHT) ?
         VOUT_MAX_HEIGHT : vout_height;
   vout_offset = (vout_offset > vout_width * (VOUT_MAX_HEIGHT - 1) * 2) ?
         vout_width * (VOUT_MAX_HEIGHT - 1) * 2 : vout_offset;
#endif
   Pico.m.dirtyPal = 1;

   // Update the geometry
   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

void emu_32x_startup(void)
{
   PicoDrawSetOutFormat(vout_format, 0);
   if ((vm_current_start_line != -1) &&
       (vm_current_line_count != -1) &&
       (vm_current_is_32cols != -1))
      emu_video_mode_change(
            vm_current_start_line,
            vm_current_line_count,
            vm_current_is_32cols);
}

void lprintf(const char *fmt, ...)
{
   char buffer[256];
   va_list ap;
   va_start(ap, fmt);
   vsprintf(buffer, fmt, ap);
   /* TODO - add 'level' param for warning/error messages? */
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s", buffer);
   va_end(ap);
}

/* libretro */
bool libretro_supports_bitmasks = false;

void retro_set_environment(retro_environment_t cb)
{
#ifdef USE_LIBRETRO_VFS
   struct retro_vfs_interface_info vfs_iface_info;
#endif

   static const struct retro_system_content_info_override content_overrides[] = {
      {
         "gen|smd|md|32x|sms|68k|sgd", /* extensions */
#if defined(LOW_MEMORY)
         true,                         /* need_fullpath */
#else
         false,                        /* need_fullpath */
#endif
         false                         /* persistent_data */
      },
      { NULL, false, false }
   };

   environ_cb = cb;

   libretro_set_core_options(environ_cb);
   environ_cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE,
         (void*)content_overrides);

#ifdef USE_LIBRETRO_VFS
   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);
#endif
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name = "PicoDrive";
#ifndef GIT_VERSION
#define _GIT_VERSION ""
#else
#define _GIT_VERSION "-" GIT_VERSION
#endif
   info->library_version = VERSION _GIT_VERSION;
   info->valid_extensions = "bin|gen|smd|md|32x|cue|iso|chd|sms|m3u|68k|sgd";
   info->need_fullpath = true;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float common_width;

   memset(info, 0, sizeof(*info));
   info->timing.fps            = Pico.m.pal ? 50 : 60;
   info->timing.sample_rate    = PicoIn.sndRate;
   info->geometry.base_width   = vout_width;
   info->geometry.base_height  = vout_height;
   info->geometry.max_width    = vout_width;
   info->geometry.max_height   = vout_height;

   common_width = vout_width;
   if (user_vout_width != 0)
      common_width = user_vout_width;

   info->geometry.aspect_ratio = common_width / vout_height;
}

/* savestates */
struct savestate_state {
   const char *load_buf;
   char *save_buf;
   size_t size;
   size_t pos;
};

size_t state_read(void *p, size_t size, size_t nmemb, void *file)
{
   struct savestate_state *state = file;
   size_t bsize = size * nmemb;

   if (state->pos + bsize > state->size) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "savestate error: %u/%u\n",
               state->pos + bsize, state->size);
      bsize = state->size - state->pos;
      if ((int)bsize <= 0)
         return 0;
   }

   memcpy(p, state->load_buf + state->pos, bsize);
   state->pos += bsize;
   return bsize;
}

size_t state_write(void *p, size_t size, size_t nmemb, void *file)
{
   struct savestate_state *state = file;
   size_t bsize = size * nmemb;

   if (state->pos + bsize > state->size) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "savestate error: %u/%u\n",
               state->pos + bsize, state->size);
      bsize = state->size - state->pos;
      if ((int)bsize <= 0)
         return 0;
   }

   memcpy(state->save_buf + state->pos, p, bsize);
   state->pos += bsize;
   return bsize;
}

size_t state_skip(void *p, size_t size, size_t nmemb, void *file)
{
   struct savestate_state *state = file;
   size_t bsize = size * nmemb;

   state->pos += bsize;
   return bsize;
}

size_t state_eof(void *file)
{
   struct savestate_state *state = file;

   return state->pos >= state->size;
}

int state_fseek(void *file, long offset, int whence)
{
   struct savestate_state *state = file;

   switch (whence) {
   case SEEK_SET:
      state->pos = offset;
      break;
   case SEEK_CUR:
      state->pos += offset;
      break;
   case SEEK_END:
      state->pos = state->size + offset;
      break;
   }
   return (int)state->pos;
}

/* savestate sizes vary wildly depending if cd/32x or
 * carthw is active, so run the whole thing to get size */
size_t retro_serialize_size(void)
{
   struct savestate_state state = { 0, };
   int ret;

   ret = PicoStateFP(&state, 1, NULL, state_skip, NULL, state_fseek);
   if (ret != 0)
      return 0;

   return state.pos;
}

bool retro_serialize(void *data, size_t size)
{
   struct savestate_state state = { 0, };
   int ret;

   state.save_buf = data;
   state.size = size;
   state.pos = 0;

   ret = PicoStateFP(&state, 1, NULL, state_write,
      NULL, state_fseek);
   return ret == 0;
}

bool retro_unserialize(const void *data, size_t size)
{
   struct savestate_state state = { 0, };
   int ret;

   state.load_buf = data;
   state.size = size;
   state.pos = 0;

   ret = PicoStateFP(&state, 0, state_read, NULL,
      state_eof, state_fseek);
   return ret == 0;
}

typedef struct patch
{
	unsigned int addr;
	unsigned short data;
	unsigned char comp;
} patch;

extern void decode(char *buff, patch *dest);
extern uint16_t m68k_read16(uint32_t a);
extern void m68k_write16(uint32_t a, uint16_t d);

void retro_cheat_reset(void)
{
	int i=0;
	unsigned int addr;

	for (i = 0; i < PicoPatchCount; i++)
	{
		addr = PicoPatches[i].addr;
		if (addr < Pico.romsize) {
			if (PicoPatches[i].active)
				*(unsigned short *)(Pico.rom + addr) = PicoPatches[i].data_old;
		} else {
			if (PicoPatches[i].active)
				m68k_write16(PicoPatches[i].addr,PicoPatches[i].data_old);
		}
	}

	PicoPatchUnload();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	patch pt;
	int array_len = PicoPatchCount;
	char codeCopy[256];
	char *buff;

	if (*code == '\0')
      return;
	strcpy(codeCopy, code);
	buff = strtok(codeCopy,"+");

	while (buff != NULL)
	{
		decode(buff, &pt);
		if (pt.addr == (uint32_t) -1 || pt.data == (uint16_t) -1)
		{
			log_cb(RETRO_LOG_ERROR,"CHEATS: Invalid code: %s\n",buff);
			return;
		}

		/* code was good, add it */
		if (array_len < PicoPatchCount + 1)
		{
			void *ptr;
			array_len *= 2;
			array_len++;
			ptr = realloc(PicoPatches, array_len * sizeof(PicoPatches[0]));
			if (ptr == NULL) {
				log_cb(RETRO_LOG_ERROR,"CHEATS: Failed to allocate memory for: %s\n",buff);
				return;
			}
			PicoPatches = ptr;
		}
		strcpy(PicoPatches[PicoPatchCount].code, buff);

		PicoPatches[PicoPatchCount].active = enabled;
		PicoPatches[PicoPatchCount].addr = pt.addr;
		PicoPatches[PicoPatchCount].data = pt.data;
		PicoPatches[PicoPatchCount].comp = pt.comp;
		if (PicoPatches[PicoPatchCount].addr < Pico.romsize)
			PicoPatches[PicoPatchCount].data_old = *(uint16_t *)(Pico.rom + PicoPatches[PicoPatchCount].addr);
		else
			PicoPatches[PicoPatchCount].data_old = (uint16_t) m68k_read16(PicoPatches[PicoPatchCount].addr);
		PicoPatchCount++;

		buff = strtok(NULL,"+");
	}
}

/* multidisk support */
static unsigned int disk_initial_index;
static bool disk_ejected;
static unsigned int disk_current_index;
static unsigned int disk_count;
static char disk_initial_path[PATH_MAX];
static struct disks_state {
   char *fname;
   char *flabel;
} disks[8];

static void get_disk_label(char *disk_label, const char *disk_path, size_t len)
{
   const char *base = NULL;

   if (!disk_path || (*disk_path == '\0'))
      return;

   base = strrchr(disk_path, SLASH);
   if (!base)
      base = disk_path;

   if (*base == SLASH)
      base++;

   strncpy(disk_label, base, len - 1);
   disk_label[len - 1] = '\0';

   char *ext = strrchr(disk_label, '.');
   if (ext)
      *ext = '\0';
}

static void disk_init(void)
{
   size_t i;

   disk_ejected       = false;
   disk_current_index = 0;
   disk_count         = 0;

   for (i = 0; i < sizeof(disks) / sizeof(disks[0]); i++)
   {
      if (disks[i].fname != NULL)
      {
         free(disks[i].fname);
         disks[i].fname = NULL;
      }
      if (disks[i].flabel != NULL)
      {
         free(disks[i].flabel);
         disks[i].flabel = NULL;
      }
   }
}

static bool disk_set_eject_state(bool ejected)
{
   // TODO?
   disk_ejected = ejected;
   return true;
}

static bool disk_get_eject_state(void)
{
   return disk_ejected;
}

static unsigned int disk_get_image_index(void)
{
   return disk_current_index;
}

static bool disk_set_image_index(unsigned int index)
{
   enum cd_track_type cd_type;
   int ret;

   if (index >= sizeof(disks) / sizeof(disks[0]))
      return false;

   if (disks[index].fname == NULL) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "missing disk #%u\n", index);

      // RetroArch specifies "no disk" with index == count,
      // so don't fail here..
      disk_current_index = index;
      return true;
   }

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "switching to disk %u: \"%s\"\n", index,
            disks[index].fname);

   ret = -1;
   cd_type = PicoCdCheck(disks[index].fname, NULL);
   if (cd_type >= 0 && cd_type != CT_UNKNOWN)
      ret = cdd_load(disks[index].fname, cd_type);
   if (ret != 0) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Load failed, invalid CD image?\n");
      return 0;
   }

   disk_current_index = index;
   return true;
}

static unsigned int disk_get_num_images(void)
{
   return disk_count;
}

static bool disk_replace_image_index(unsigned index,
   const struct retro_game_info *info)
{
   char *old_fname  = NULL;
   char *old_flabel = NULL;
   bool ret         = true;

   if (index >= sizeof(disks) / sizeof(disks[0]))
      return false;

   old_fname  = disks[index].fname;
   old_flabel = disks[index].flabel;

   if (disks[index].fname != NULL)
      free(disks[index].fname);
   disks[index].fname = NULL;

   if (disks[index].flabel != NULL)
      free(disks[index].flabel);
   disks[index].flabel = NULL;

   if (info != NULL) {
      char disk_label[PATH_MAX];
      disk_label[0] = '\0';

      disks[index].fname = strdup(info->path);

      get_disk_label(disk_label, info->path, PATH_MAX);
      disks[index].flabel = strdup(disk_label);
      
      if (index == disk_current_index)
         ret = disk_set_image_index(index);
   }

   if (old_fname != NULL)
      free(old_fname);

   if (old_flabel != NULL)
      free(old_flabel);

   return ret;
}

static bool disk_add_image_index(void)
{
   if (disk_count >= sizeof(disks) / sizeof(disks[0]))
      return false;

   disk_count++;
   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
   if (index >= sizeof(disks) / sizeof(disks[0]))
      return false;

   if (!path || (*path == '\0'))
      return false;

   disk_initial_index = index;

   strncpy(disk_initial_path, path, sizeof(disk_initial_path) - 1);
   disk_initial_path[sizeof(disk_initial_path) - 1] = '\0';

   return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
   const char *fname = NULL;

   if (len < 1)
      return false;

   if (index >= sizeof(disks) / sizeof(disks[0]))
      return false;

   fname = disks[index].fname;

   if (!fname || (*fname == '\0'))
      return false;

   strncpy(path, fname, len - 1);
   path[len - 1] = '\0';

   return true;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
   const char *flabel = NULL;

   if (len < 1)
      return false;

   if (index >= sizeof(disks) / sizeof(disks[0]))
      return false;

   flabel = disks[index].flabel;

   if (!flabel || (*flabel == '\0'))
      return false;

   strncpy(label, flabel, len - 1);
   label[len - 1] = '\0';

   return true;
}

static struct retro_disk_control_callback disk_control = {
   disk_set_eject_state,
   disk_get_eject_state,
   disk_get_image_index,
   disk_set_image_index,
   disk_get_num_images,
   disk_replace_image_index,
   disk_add_image_index,
};

static struct retro_disk_control_ext_callback disk_control_ext = {
   .set_eject_state     = disk_set_eject_state,
   .get_eject_state     = disk_get_eject_state,
   .get_image_index     = disk_get_image_index,
   .set_image_index     = disk_set_image_index,
   .get_num_images      = disk_get_num_images,
   .replace_image_index = disk_replace_image_index,
   .add_image_index     = disk_add_image_index,
   .set_initial_image   = disk_set_initial_image,
   .get_image_path      = disk_get_image_path,
   .get_image_label     = disk_get_image_label,
};

static void disk_tray_open(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "cd tray open\n");
   disk_ejected = 1;
}

static void disk_tray_close(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "cd tray close\n");
   disk_ejected = 0;
}

static char base_dir[1024];

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static bool read_m3u(const char *file)
{
   char line[1024];
   char name[PATH_MAX];
   FILE *f = fopen(file, "r");
   if (!f)
      return false;

   while (fgets(line, sizeof(line), f) && disk_count < sizeof(disks) / sizeof(disks[0]))
   {
      if (line[0] == '#')
         continue;
      char *carrige_return = strchr(line, '\r');
      if (carrige_return)
         *carrige_return = '\0';
      char *newline = strchr(line, '\n');
      if (newline)
         *newline = '\0';

      if (line[0] != '\0')
      {
         char disk_label[PATH_MAX];
         disk_label[0] = '\0';

         snprintf(name, sizeof(name), "%s%c%s", base_dir, SLASH, line);
         disks[disk_count].fname = strdup(name);

         get_disk_label(disk_label, name, PATH_MAX);
         disks[disk_count].flabel = strdup(disk_label);

         disk_count++;
      }
   }

   fclose(f);
   return (disk_count != 0);
}

/* end of multi disk support */

static const char * const biosfiles_us[] = {
   "us_scd2_9306", "SegaCDBIOS9303", "us_scd1_9210", "bios_CD_U"
};
static const char * const biosfiles_eu[] = {
   "eu_mcd2_9306", "eu_mcd2_9303", "eu_mcd1_9210", "bios_CD_E"
};
static const char * const biosfiles_jp[] = {
   "jp_mcd2_921222", "jp_mcd1_9112", "jp_mcd1_9111", "bios_CD_J"
};

static void make_system_path(char *buf, size_t buf_size,
   const char *name, const char *ext)
{
   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
      snprintf(buf, buf_size, "%s%c%s%s", dir, SLASH, name, ext);
   }
   else {
      snprintf(buf, buf_size, "%s%s", name, ext);
   }
}

static const char *find_bios(int *region, const char *cd_fname)
{
   const char * const *files;
   static char path[256];
   int i, count;
   FILE *f = NULL;

   if (*region == 4) { // US
      files = biosfiles_us;
      count = sizeof(biosfiles_us) / sizeof(char *);
   } else if (*region == 8) { // EU
      files = biosfiles_eu;
      count = sizeof(biosfiles_eu) / sizeof(char *);
   } else if (*region == 1 || *region == 2) {
      files = biosfiles_jp;
      count = sizeof(biosfiles_jp) / sizeof(char *);
   } else {
      return NULL;
   }

   for (i = 0; i < count; i++)
   {
      make_system_path(path, sizeof(path), files[i], ".bin");
      f = fopen(path, "rb");
      if (f != NULL)
         break;

      make_system_path(path, sizeof(path), files[i], ".zip");
      f = fopen(path, "rb");
      if (f != NULL)
         break;
   }

   if (f != NULL) {
      if (log_cb)
         log_cb(RETRO_LOG_INFO, "using bios: %s\n", path);
      fclose(f);
      return path;
   }

   return NULL;
}

static void set_memory_maps(void)
{
   if (PicoIn.AHW & PAHW_MCD)
   {
      const size_t SCD_BIT = 1ULL << 31ULL;
      const uint64_t mem = RETRO_MEMDESC_SYSTEM_RAM;
      struct retro_memory_map mmaps;
      struct retro_memory_descriptor descs[] = {
         { mem, PicoMem.ram,        0,           0xFF0000, 0, 0, 0x10000, "68KRAM" },
         /* virtual address using SCD_BIT so all 512M of prg_ram can be accessed */
         /* at address $80020000 */
         { mem, Pico_mcd->prg_ram,  0, SCD_BIT | 0x020000, 0, 0, 0x80000, "PRGRAM" },
      };

      mmaps.descriptors = descs;
      mmaps.num_descriptors = sizeof(descs) / sizeof(descs[0]);
      environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
   }
}

bool retro_load_game(const struct retro_game_info *info)
{
   const struct retro_game_info_ext *info_ext = NULL;
   const unsigned char *content_data          = NULL;
   size_t content_size                        = 0;
   char content_path[PATH_MAX];
   char content_ext[8];
   char carthw_path[PATH_MAX];
   enum media_type_e media_type;
   size_t i;

#if defined(_WIN32)
   char slash      = '\\';
#else
   char slash      = '/';
#endif

   content_path[0] = '\0';
   content_ext[0]  = '\0';
   carthw_path[0]  = '\0';

   unsigned int cd_index = 0;
   bool is_m3u           = false;

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "C" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Mode" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "C" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "X" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Mode" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },

      { 0 },
   };

   struct retro_input_descriptor desc_sms[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1 Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Button Pause" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1 Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Button Pause" },

      { 0 },
   };

   /* Attempt to fetch extended game info */
   if (environ_cb(RETRO_ENVIRONMENT_GET_GAME_INFO_EXT, &info_ext))
   {
#if !defined(LOW_MEMORY)
      content_data = (const unsigned char *)info_ext->data;
      content_size = info_ext->size;
#endif
      strncpy(base_dir, info_ext->dir, sizeof(base_dir));
      base_dir[sizeof(base_dir) - 1] = '\0';

      strncpy(content_ext, info_ext->ext, sizeof(content_ext));
      content_ext[sizeof(content_ext) - 1] = '\0';

      if (info_ext->file_in_archive)
      {
         /* We don't have a 'physical' file in this
          * case, but the core still needs a filename
          * in order to detect media type. We therefore
          * fake it, using the content directory,
          * canonical content name, and content file
          * extension */
         snprintf(content_path, sizeof(content_path), "%s%c%s.%s",
               base_dir, slash, info_ext->name, content_ext);
      }
      else
      {
         strncpy(content_path, info_ext->full_path, sizeof(content_path));
         content_path[sizeof(content_path) - 1] = '\0';
      }
   }
   else
   {
      const char *ext = NULL;

      if (!info || !info->path)
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "info->path required\n");
         return false;
      }

      extract_directory(base_dir, info->path, sizeof(base_dir));

      strncpy(content_path, info->path, sizeof(content_path));
      content_path[sizeof(content_path) - 1] = '\0';

      if ((ext = strrchr(info->path, '.')))
      {
         strncpy(content_ext, ext + 1, sizeof(content_ext));
         content_ext[sizeof(content_ext) - 1] = '\0';
      }
   }

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "RGB565 support required, sorry\n");
      return false;
   }

   disk_init();

   is_m3u = (strcasestr(content_ext, "m3u") != NULL);
   if (is_m3u)
   {
      if (!read_m3u(content_path))
      {
         log_cb(RETRO_LOG_INFO, "failed to read m3u file\n");
         return false;
      }

      strncpy(content_path, disks[0].fname, sizeof(content_path));
      content_path[sizeof(content_path) - 1] = '\0';
   }
   else
   {
      char disk_label[PATH_MAX];
      disk_label[0] = '\0';

      disk_current_index = 0;
      disk_count = 1;
      disks[0].fname = strdup(content_path);

      get_disk_label(disk_label, content_path, PATH_MAX);
      disks[0].flabel = strdup(disk_label);
   }

   /* If this is an M3U file, attempt to set the
    * initial disk image */
   if (is_m3u && (disk_initial_index > 0) && (disk_initial_index < disk_count))
   {
      const char *fname = disks[disk_initial_index].fname;

      if (fname && (*fname != '\0'))
         if (strcmp(disk_initial_path, fname) == 0)
            cd_index = disk_initial_index;

      /* If we are not loading the first disk in the
       * M3U list, update the content_path string
       * that will be passed to PicoLoadMedia() */
      if (cd_index != 0)
      {
         strncpy(content_path, disks[cd_index].fname, sizeof(content_path));
         content_path[sizeof(content_path) - 1] = '\0';
      }
   }

   make_system_path(carthw_path, sizeof(carthw_path), "carthw", ".cfg");

   media_type = PicoLoadMedia(content_path, content_data, content_size,
         carthw_path, find_bios, NULL);

   disk_current_index = cd_index;

   switch (media_type) {
   case PM_BAD_DETECT:
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Failed to detect ROM/CD image type.\n");
      return false;
   case PM_BAD_CD:
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Invalid CD image\n");
      return false;
   case PM_BAD_CD_NO_BIOS:
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Missing BIOS\n");
      return false;
   case PM_ERROR:
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Load error\n");
      return false;
   default:
      break;
   }

   if (media_type == PM_MARK3)
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc_sms);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   PicoLoopPrepare();

   PicoIn.writeSound = snd_write;
   memset(sndBuffer, 0, sizeof(sndBuffer));
   PicoIn.sndOut = sndBuffer;
   PsndRerate(0);

   PicoDrawSetOutFormat(vout_format, 0);

   /* Setup retro memory maps */
   set_memory_maps();

   init_frameskip();

   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   return false;
}

void retro_unload_game(void)
{
}

unsigned retro_get_region(void)
{
   return Pico.m.pal ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

void *retro_get_memory_data(unsigned type)
{
   uint8_t* data;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         /* Note: MCD RAM cart uses Pico.sv.data */
         if ((PicoIn.AHW & PAHW_MCD) &&
               !(PicoIn.opt & POPT_EN_MCD_RAMCART))
            data = Pico_mcd->bram;
         else
            data = Pico.sv.data;
         break;
      case RETRO_MEMORY_SYSTEM_RAM:
         if (PicoIn.AHW & PAHW_SMS)
            data = PicoMem.zram;
         else
            data = PicoMem.ram;
         break;
      default:
         data = NULL;
         break;
   }

   return data;
}

size_t retro_get_memory_size(unsigned type)
{
   unsigned int i;
   int sum;

   switch(type)
   {
      case RETRO_MEMORY_SAVE_RAM:
         if (PicoIn.AHW & PAHW_MCD)
         {
            if (PicoIn.opt & POPT_EN_MCD_RAMCART)
               return 0x12000;
            else /* bram */
               return 0x2000;
         }

         if (Pico.m.frame_count == 0)
            return Pico.sv.size;

         // if game doesn't write to sram, don't report it to
         // libretro so that RA doesn't write out zeroed .srm
         for (i = 0, sum = 0; i < Pico.sv.size; i++)
            sum |= Pico.sv.data[i];

         return (sum != 0) ? Pico.sv.size : 0;

      case RETRO_MEMORY_SYSTEM_RAM:
         if (PicoIn.AHW & PAHW_SMS)
            return 0x2000;
         else
            return sizeof(PicoMem.ram);

      default:
         return 0;
   }

}

void retro_reset(void)
{
   PicoReset();
}

static const unsigned short retro_pico_map[] = {
   1 << GBTN_B,
   1 << GBTN_A,
   1 << GBTN_MODE,
   1 << GBTN_START,
   1 << GBTN_UP,
   1 << GBTN_DOWN,
   1 << GBTN_LEFT,
   1 << GBTN_RIGHT,
   1 << GBTN_C,
   1 << GBTN_Y,
   1 << GBTN_X,
   1 << GBTN_Z,
};
#define RETRO_PICO_MAP_LEN (sizeof(retro_pico_map) / sizeof(retro_pico_map[0]))

static void snd_write(int len)
{
   audio_batch_cb(PicoIn.sndOut, len / 4);
}

static enum input_device input_name_to_val(const char *name)
{
   if (strcmp(name, "3 button pad") == 0)
      return PICO_INPUT_PAD_3BTN;
   if (strcmp(name, "6 button pad") == 0)
      return PICO_INPUT_PAD_6BTN;
   if (strcmp(name, "None") == 0)
      return PICO_INPUT_NOTHING;

   if (log_cb)
      log_cb(RETRO_LOG_WARN, "invalid picodrive_input: '%s'\n", name);
   return PICO_INPUT_PAD_3BTN;
}

static void update_variables(bool first_run)
{
   struct retro_variable var;
   int OldPicoRegionOverride;
   float old_user_vout_width;
   unsigned old_frameskip_type;
   int old_vout_format;
   double new_sound_rate;
   unsigned short old_snd_filter;
   int32_t old_snd_filter_range;

   var.value = NULL;
   var.key = "picodrive_input1";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      PicoSetInputDevice(0, input_name_to_val(var.value));

   var.value = NULL;
   var.key = "picodrive_input2";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      PicoSetInputDevice(1, input_name_to_val(var.value));

   var.value = NULL;
   var.key = "picodrive_sprlim";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "enabled") == 0)
         PicoIn.opt |= POPT_DIS_SPRITE_LIM;
      else
         PicoIn.opt &= ~POPT_DIS_SPRITE_LIM;
   }

   var.value = NULL;
   var.key = "picodrive_ramcart";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "enabled") == 0)
         PicoIn.opt |= POPT_EN_MCD_RAMCART;
      else
         PicoIn.opt &= ~POPT_EN_MCD_RAMCART;
   }

   OldPicoRegionOverride = PicoIn.regionOverride;
   var.value = NULL;
   var.key = "picodrive_region";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "Auto") == 0)
         PicoIn.regionOverride = 0;
      else if (strcmp(var.value, "Japan NTSC") == 0)
         PicoIn.regionOverride = 1;
      else if (strcmp(var.value, "Japan PAL") == 0)
         PicoIn.regionOverride = 2;
      else if (strcmp(var.value, "US") == 0)
         PicoIn.regionOverride = 4;
      else if (strcmp(var.value, "Europe") == 0)
         PicoIn.regionOverride = 8;
   }

   // Update region, fps and sound flags if needed
   if (Pico.rom && PicoIn.regionOverride != OldPicoRegionOverride)
   {
      PicoDetectRegion();
      PicoLoopPrepare();
      PsndRerate(1);
   }

   old_user_vout_width = user_vout_width;
   var.value = NULL;
   var.key = "picodrive_aspect";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "4/3") == 0)
         user_vout_width = VOUT_4_3;
      else if (strcmp(var.value, "CRT") == 0)
         user_vout_width = VOUT_CRT;
      else
         user_vout_width = VOUT_PAR;
   }

   if (user_vout_width != old_user_vout_width)
   {
      // Update the geometry
      struct retro_system_av_info av_info;
      retro_get_system_av_info(&av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
   }

   old_show_overscan = show_overscan;
   var.value = NULL;
   var.key = "picodrive_overscan";
   show_overscan = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "enabled") == 0)
         show_overscan = true;
   }

   var.value = NULL;
   var.key = "picodrive_overclk68k";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      PicoIn.overclockM68k = 0;
      if (var.value[0] == '+')
         PicoIn.overclockM68k = atoi(var.value + 1);
   }

#ifdef DRC_SH2
   var.value = NULL;
   var.key = "picodrive_drc";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "enabled") == 0)
         PicoIn.opt |= POPT_EN_DRC;
      else
         PicoIn.opt &= ~POPT_EN_DRC;
   }
#endif
#ifdef _3DS
   if(!ctr_svchack_successful)
      PicoIn.opt &= ~POPT_EN_DRC;
#endif

   old_snd_filter = PicoIn.opt & POPT_EN_SNDFILTER;
   var.value = NULL;
   var.key = "picodrive_audio_filter";
   PicoIn.opt &= ~POPT_EN_SNDFILTER;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "low-pass") == 0)
         PicoIn.opt |= POPT_EN_SNDFILTER;
   }

   old_snd_filter_range = PicoIn.sndFilterAlpha;
   var.value = NULL;
   var.key = "picodrive_lowpass_range";
   PicoIn.sndFilterAlpha = (60 * 0x10000 / 100);
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      PicoIn.sndFilterAlpha = (atoi(var.value) * 0x10000 / 100);
   }

   if (((old_snd_filter ^ PicoIn.opt) & POPT_EN_SNDFILTER) ||
         old_snd_filter_range != PicoIn.sndFilterAlpha) {
      mix_reset (PicoIn.opt & POPT_EN_SNDFILTER ? PicoIn.sndFilterAlpha : 0);
   }

   old_frameskip_type = frameskip_type;
   frameskip_type     = 0;
   var.key            = "picodrive_frameskip";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "auto") == 0)
         frameskip_type = 1;
      else if (strcmp(var.value, "manual") == 0)
         frameskip_type = 2;
   }

   frameskip_threshold = 33;
   var.key             = "picodrive_frameskip_threshold";
   var.value           = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      frameskip_threshold = strtol(var.value, NULL, 10);

   old_vout_format = vout_format;
   var.value = NULL;
   var.key = "picodrive_renderer";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (strcmp(var.value, "fast") == 0)
         vout_format = PDF_NONE;
      else if (strcmp(var.value, "good") == 0)
         vout_format = PDF_8BIT;
      else if (strcmp(var.value, "accurate") == 0)
         vout_format = PDF_RGB555;

      PicoIn.opt &= ~POPT_ALT_RENDERER;
      if (vout_format == PDF_NONE)
         PicoIn.opt |= POPT_ALT_RENDERER;
      PicoDrawSetOutFormat(vout_format, 0);
   }

   var.value = NULL;
   var.key = "picodrive_sound_rate";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      new_sound_rate = atoi(var.value);
      if (new_sound_rate != PicoIn.sndRate) {
         /* Update the sound rate */
         PicoIn.sndRate = new_sound_rate;
         PsndRerate(1);
         struct retro_system_av_info av_info;
         retro_get_system_av_info(&av_info);
         environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
      }
   }

   /* setup video if required */
   if (show_overscan != old_show_overscan || vout_format != old_vout_format)
   {
      if ((vm_current_start_line != -1) &&
          (vm_current_line_count != -1) &&
          (vm_current_is_32cols != -1))
         emu_video_mode_change(
               vm_current_start_line,
               vm_current_line_count,
               vm_current_is_32cols);
   }

   /* Reinitialise frameskipping, if required */
   if (((frameskip_type != old_frameskip_type) ||
        (Pico.rom && PicoIn.regionOverride != OldPicoRegionOverride)) &&
       !first_run)
      init_frameskip();
}

void retro_run(void)
{
   bool updated = false;
   int pad, i;
   static void *buff;

   PicoIn.skipFrame = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   input_poll_cb();

   PicoIn.pad[0] = PicoIn.pad[1] = 0;

   if (libretro_supports_bitmasks)
   {
      for (pad = 0; pad < 2; pad++)
      {
         int16_t input = input_state_cb(
               pad, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         for (i = 0; i < RETRO_PICO_MAP_LEN; i++)
            if (input & (1 << i))
               PicoIn.pad[pad] |= retro_pico_map[i];
      }
   }
   else
   {
      for (pad = 0; pad < 2; pad++)
      {
         for (i = 0; i < RETRO_PICO_MAP_LEN; i++)
            if (input_state_cb(pad, RETRO_DEVICE_JOYPAD, 0, i))
               PicoIn.pad[pad] |= retro_pico_map[i];
      }
   }

   if (PicoPatches)
      PicoPatchApply();

   /* Check whether current frame should
    * be skipped */
   if ((frameskip_type > 0) && retro_audio_buff_active) {
      switch (frameskip_type)
      {
         case 1: /* auto */
            PicoIn.skipFrame = retro_audio_buff_underrun ? 1 : 0;
            break;
         case 2: /* manual */
            PicoIn.skipFrame = (retro_audio_buff_occupancy < frameskip_threshold) ? 1 : 0;
            break;
         default:
            PicoIn.skipFrame = 0;
            break;
      }

      if (!PicoIn.skipFrame || (frameskip_counter >= FRAMESKIP_MAX)) {
         PicoIn.skipFrame  = 0;
         frameskip_counter = 0;
      } else
         frameskip_counter++;
   }

   /* If frameskip settings have changed, update
    * frontend audio latency */
   if (update_audio_latency) {
      environ_cb(RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY,
            &audio_latency);
      update_audio_latency = false;
   }

   PicoFrame();

   /* If frame was skipped, call video_cb() with
    * a NULL buffer and return immediately */
   if (PicoIn.skipFrame) {
      video_cb(NULL, vout_width, vout_height, vout_width * 2);
      return;
   }

#if defined(RENDER_GSKIT_PS2)
   buff = (uint32_t *)RETRO_HW_FRAME_BUFFER_VALID;

   if (!ps2) {
      // get access to the graphics hardware
      if (!environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, (void **)&ps2) || !ps2) {
         printf("Failed to get HW rendering interface!\n");
         return;
      }

      if (ps2->interface_version != RETRO_HW_RENDER_INTERFACE_GSKIT_PS2_VERSION) {
         printf("HW render interface mismatch, expected %u, got %u!\n", 
                  RETRO_HW_RENDER_INTERFACE_GSKIT_PS2_VERSION, ps2->interface_version);
         return;
      }

      ps2->coreTexture->ClutPSM = GS_PSM_CT16;
      ps2->coreTexture->Filter = GS_FILTER_LINEAR;
      ps2->coreTexture->Clut = retro_palette;

      ps2->coreTexture->Mem = vout_buf;
      ps2->coreTexture->Width = vout_width;
      ps2->coreTexture->Height = vout_height;
      ps2->coreTexture->PSM = (vout_16bit ? GS_PSM_CT16 : GS_PSM_T8);
      ps2->padding = padding;
   }

   // CLUT update needed?
   if (!vout_16bit && Pico.m.dirtyPal) {
      PicoDrawUpdateHighPal();

      // Rotate CLUT. PS2 is special since entries in CLUT are not in sequence.
      unsigned short int *pal=(void *)retro_palette;
      for (i = 0; i < 256; i+=8) {
         if ((i&0x18) == 0x08)
            memcpy(pal+i,Pico.est.HighPal+i+8,16);
         else if ((i&0x18) == 0x10)
            memcpy(pal+i,Pico.est.HighPal+i-8,16);
         else
            memcpy(pal+i,Pico.est.HighPal+i,16);
      }
   }
#else
   if (!vout_16bit) {
      /* The 8 bit renderers write a CLUT image in Pico.est.Draw2FB, while libretro wants RGB in vout_buf.
       * We need to manually copy that to vout_buf, applying the CLUT on the way. Especially
       * with the fast renderer this is improving performance, at the expense of accuracy.
       */
      /* This section is mostly copied from pemu_finalize_frame in platform/linux/emu.c */
      unsigned short *pd = (unsigned short *)vout_buf;
      /* Skip the leftmost 8 columns (it is used as an overlap area for rendering) */
      unsigned char *ps = Pico.est.Draw2FB + 8;
      unsigned short *pal = Pico.est.HighPal;
      int x;
      if (Pico.m.dirtyPal)
         PicoDrawUpdateHighPal();
      /* Copy up to the max height to include the overscan area, and skip the leftmost 8 columns again */
      for (i = 0; i < VOUT_MAX_HEIGHT; i++, ps += 8) {
         for (x = 0; x < vout_width; x+=4) {
            *pd++ = pal[*ps++];
            *pd++ = pal[*ps++];
            *pd++ = pal[*ps++];
            *pd++ = pal[*ps++];
         }
         ps += 320-vout_width; /* Advance to next line in case of 32col mode */
      }
   }

   buff = (char*)vout_buf + vout_offset;
#endif

	video_cb((short *)buff, vout_width, vout_height, vout_width * 2);
}

void retro_init(void)
{
   unsigned dci_version = 0;
   struct retro_log_callback log;
   int level;

   level = 0;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   disk_initial_index = 0;
   disk_initial_path[0] = '\0';
   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &dci_version) && (dci_version >= 1))
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &disk_control_ext);
   else
      environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_control);

#ifdef _3DS
   ctr_svchack_successful = ctr_svchack_init();
   check_rosalina();
#elif defined(VITA)
   sceBlock = getVMBlock();
#endif

   PicoIn.opt = POPT_EN_STEREO|POPT_EN_FM|POPT_EN_PSG|POPT_EN_Z80|POPT_EN_YM2413
      | POPT_EN_MCD_PCM|POPT_EN_MCD_CDDA|POPT_EN_MCD_GFX
      | POPT_EN_32X|POPT_EN_PWM
      | POPT_ACC_SPRITES|POPT_DIS_32C_BORDER;
#ifdef __arm__
#ifdef _3DS
   if (ctr_svchack_successful)
#endif
      PicoIn.opt |= POPT_EN_DRC;
#endif

   struct retro_variable var = { .key = "picodrive_sound_rate" };
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      PicoIn.sndRate = atoi(var.value);
   else
      PicoIn.sndRate = INITIAL_SND_RATE;

   PicoIn.autoRgnOrder = 0x184; // US, EU, JP

   vout_width = VOUT_MAX_WIDTH;
   vout_height = VOUT_MAX_HEIGHT;
#ifdef _3DS
   vout_buf = linearMemAlign(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2, 0x80);
#elif defined(RENDER_GSKIT_PS2)
   vout_buf = memalign(4096, VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
   retro_palette = memalign(128, gsKit_texture_size_ee(16, 16, GS_PSM_CT16));
#else
   vout_buf = malloc(VOUT_MAX_WIDTH * VOUT_MAX_HEIGHT * 2);
#endif

   PicoInit();

   //PicoIn.osdMessage = plat_status_msg_busy_next;
   PicoIn.mcdTrayOpen = disk_tray_open;
   PicoIn.mcdTrayClose = disk_tray_close;

   frameskip_type             = 0;
   frameskip_threshold        = 0;
   frameskip_counter          = 0;
   retro_audio_buff_active    = false;
   retro_audio_buff_occupancy = 0;
   retro_audio_buff_underrun  = false;
   audio_latency              = 0;
   update_audio_latency       = false;

   update_variables(true);
}

void retro_deinit(void)
{
#ifdef _3DS
   linearFree(vout_buf);
#elif defined(RENDER_GSKIT_PS2)
   free(vout_buf);
   free(retro_palette);
   ps2 = NULL;
#else
   free(vout_buf);
#endif
   vout_buf = NULL;
   PicoExit();

   disk_init();

   libretro_supports_bitmasks = false;
}

//! Zakuro's PICA200 renderer (software) behind a C interface, so the
//! runtime draws until its native Vulkan renderer exists. The GPU reaches
//! the 3DS's memory by physical address, in the runtime's own buffers.
//! See bridge/zakuro-gpu.h.

use zakuro_gpu::{Gpu, GpuMemory, SoftwareRenderer};

/// the runtime's physical memories, by their physical addresses.
struct Physical {
    regions: [(u32, *mut u8, usize); 3],
}

impl Physical {
    fn find(&self, addr: u32, len: usize) -> Option<*mut u8> {
        self.regions.iter().find_map(|&(base, pointer, size)| {
            let offset = addr.wrapping_sub(base) as usize;
            (!pointer.is_null() && offset < size && offset + len <= size).then(|| unsafe { pointer.add(offset) })
        })
    }
}

impl GpuMemory for Physical {
    fn read(&mut self, addr: u32, out: &mut [u8]) {
        match self.find(addr, out.len()) {
            Some(p) => unsafe { std::ptr::copy_nonoverlapping(p, out.as_mut_ptr(), out.len()) },
            None => out.fill(0),
        }
    }

    fn write(&mut self, addr: u32, data: &[u8]) {
        if let Some(p) = self.find(addr, data.len()) {
            unsafe { std::ptr::copy_nonoverlapping(data.as_ptr(), p, data.len()) }
        }
    }

    fn slice(&mut self, addr: u32, len: usize) -> Option<&[u8]> {
        self.find(addr, len).map(|p| unsafe { std::slice::from_raw_parts(p, len) })
    }

    fn slice_mut(&mut self, addr: u32, len: usize) -> Option<&mut [u8]> {
        self.find(addr, len).map(|p| unsafe { std::slice::from_raw_parts_mut(p, len) })
    }
}

pub struct ZGpu {
    gpu: Gpu,
    renderer: SoftwareRenderer,
    memory: Physical,
}

/// # Safety
///
/// the buffers stay alive and in place as long as the GPU does.
#[no_mangle]
pub unsafe extern "C" fn zgpu_new(fcram: *mut u8, fcram_size: usize, vram: *mut u8, vram_size: usize, axi: *mut u8, axi_size: usize) -> *mut ZGpu {
    Box::into_raw(Box::new(ZGpu {
        gpu: Gpu::new(),
        renderer: SoftwareRenderer::default(),
        memory: Physical { regions: [(0x2000_0000, fcram, fcram_size), (0x1800_0000, vram, vram_size), (0x1FF8_0000, axi, axi_size)] },
    }))
}

/// # Safety
///
/// gpu came from zgpu_new and is not used after.
#[no_mangle]
pub unsafe extern "C" fn zgpu_free(gpu: *mut ZGpu) {
    if !gpu.is_null() {
        drop(unsafe { Box::from_raw(gpu) });
    }
}

fn with<R>(gpu: *mut ZGpu, f: impl FnOnce(&mut ZGpu) -> R) -> R {
    // SAFETY: the runtime hands back what zgpu_new gave it, from one thread
    f(unsafe { &mut *gpu })
}

#[no_mangle]
pub extern "C" fn zgpu_write_register(gpu: *mut ZGpu, offset: u32, value: u32) {
    with(gpu, |g| g.gpu.write_external(offset, value))
}

#[no_mangle]
pub extern "C" fn zgpu_read_register(gpu: *mut ZGpu, offset: u32) -> u32 {
    with(gpu, |g| g.gpu.read_external(offset))
}

#[no_mangle]
pub extern "C" fn zgpu_command_list(gpu: *mut ZGpu, paddr: u32, size: u32) {
    with(gpu, |g| g.gpu.process_command_list(&mut g.memory, &mut g.renderer, paddr, size))
}

#[no_mangle]
pub extern "C" fn zgpu_memory_fill(gpu: *mut ZGpu, start: u32, end: u32, value: u32, width: u32) {
    with(gpu, |g| g.gpu.memory_fill(&mut g.memory, start, end, value, width))
}

#[no_mangle]
pub extern "C" fn zgpu_display_transfer(gpu: *mut ZGpu, input: u32, output: u32, input_size: u32, output_size: u32, flags: u32) {
    with(gpu, |g| g.gpu.display_transfer(&mut g.memory, input, output, input_size, output_size, flags))
}

#[no_mangle]
pub extern "C" fn zgpu_texture_copy(gpu: *mut ZGpu, input: u32, output: u32, size: u32, input_gap: u32, output_gap: u32) {
    with(gpu, |g| g.gpu.texture_copy(&mut g.memory, input, output, size, input_gap, output_gap))
}

#[no_mangle]
pub extern "C" fn zgpu_set_framebuffer(gpu: *mut ZGpu, screen: u32, active: u32, left: u32, right: u32, stride: u32, format: u32) {
    with(gpu, |g| g.gpu.set_framebuffer(screen, active, left, right, stride, format))
}

/// the picture a screen shows now: its physical address, the stride of its
/// rows and its format (the LCD's register).
#[no_mangle]
pub extern "C" fn zgpu_framebuffer(gpu: *mut ZGpu, screen: u32, address: *mut u32, stride: *mut u32, format: *mut u32) {
    with(gpu, |g| {
        let config = g.gpu.framebuffers[(screen & 1) as usize];
        // SAFETY: the runtime passes three words to fill
        unsafe {
            *address = config.address_left();
            *stride = config.stride;
            *format = config.format;
        }
    })
}

/// draws on the host's GPU through Vulkan from now on, at scale times the
/// 3DS's resolution; 0 when it can't (software drawing goes on)
#[no_mangle]
pub extern "C" fn zgpu_enable_hardware(gpu: *mut ZGpu, scale: u32) -> i32 {
    with(gpu, |g| g.gpu.enable_hardware_renderer(scale, None, None).is_ok() as i32)
}

/// memory over a range made to hold what the host GPU drew there
#[no_mangle]
pub extern "C" fn zgpu_sync(gpu: *mut ZGpu, addr: u32, len: u32) {
    with(gpu, |g| g.gpu.sync_memory(&mut g.memory, addr, len))
}

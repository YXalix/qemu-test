/*
 * ublk-daemon - Userspace block device daemon using libublk v0.4.5
 *
 * RAM-based block device implementation.
 */

use anyhow::Result;
use libublk::io::{BufDescList, UblkDev, UblkIOCtx, UblkQueue};
use libublk::{BufDesc, UblkIORes};
use std::cell::RefCell;
use std::env;
use std::process;
use std::rc::Rc;

const DEFAULT_DEV_ID: i32 = -1; // Auto allocate
const DEFAULT_QUEUE_DEPTH: u16 = 64;
const DEFAULT_NR_QUEUES: u16 = 1;
const DEFAULT_BUF_SIZE: u32 = 524288; // 512KB

struct RamDisk {
    data: Vec<u8>,
    size: usize,
}

impl RamDisk {
    fn new(size: usize) -> Self {
        RamDisk {
            data: vec![0u8; size],
            size,
        }
    }

    fn read(&self, offset: u64, buf: &mut [u8]) -> i32 {
        let off = offset as usize;
        let len = buf.len();
        if off + len > self.size {
            return -libc::EINVAL;
        }
        buf.copy_from_slice(&self.data[off..off + len]);
        len as i32
    }

    fn write(&mut self, offset: u64, buf: &[u8]) -> i32 {
        let off = offset as usize;
        let len = buf.len();
        if off + len > self.size {
            return -libc::EINVAL;
        }
        self.data[off..off + len].copy_from_slice(buf);
        len as i32
    }
}

// Queue function
fn queue_fn(qid: u16, dev: &UblkDev, dev_size: usize) {
    let bufs = Rc::new(dev.alloc_queue_io_bufs());
    let ramdisk = Rc::new(RefCell::new(RamDisk::new(dev_size)));
    let bufs_clone = bufs.clone();

    let handler = move |q: &UblkQueue, tag: u16, _io: &UblkIOCtx| {
        let iod = q.get_iod(tag);
        let op = iod.op_flags & 0xff;
        let off = iod.start_sector << 9;
        let bytes = (iod.nr_sectors << 9) as usize;
        let io_slice = bufs_clone[tag as usize].as_slice();

        let res = match op {
            libublk::sys::UBLK_IO_OP_READ => {
                let buf = unsafe { std::slice::from_raw_parts_mut(io_slice.as_ptr() as *mut u8, bytes) };
                ramdisk.borrow().read(off, buf)
            }
            libublk::sys::UBLK_IO_OP_WRITE => {
                let buf = unsafe { std::slice::from_raw_parts(io_slice.as_ptr(), bytes) };
                ramdisk.borrow_mut().write(off, buf)
            }
            libublk::sys::UBLK_IO_OP_FLUSH => 0,
            _ => -libc::EIO,
        };

        // Use complete_io_cmd_unified (sync version) for IO completion
        let io_res = Ok(UblkIORes::Result(res));
        if let Err(e) = q.complete_io_cmd_unified(tag, BufDesc::Slice(&bufs_clone[tag as usize]), io_res) {
            eprintln!("Failed to complete IO for tag {}: {:?}", tag, e);
        }
    };

    UblkQueue::new(qid, dev)
        .unwrap()
        .submit_fetch_commands_unified(BufDescList::Slices(Some(&bufs)))
        .unwrap()
        .wait_and_handle_io(handler);
}

fn print_usage(program: &str) {
    eprintln!("Usage: {} [options]", program);
    eprintln!("Options:");
    eprintln!("  -d DEV_ID      Device ID (-1 for auto, default: -1)");
    eprintln!("  -s SIZE        Device size in MB (default: 64)");
    eprintln!("  -h, --help     Show this help");
}

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    let program = &args[0];

    let mut dev_id = DEFAULT_DEV_ID;
    let mut dev_size_mb = 64;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-d" => {
                i += 1;
                if i >= args.len() {
                    print_usage(program);
                    process::exit(1);
                }
                dev_id = args[i].parse().unwrap_or(DEFAULT_DEV_ID);
            }
            "-s" => {
                i += 1;
                if i >= args.len() {
                    print_usage(program);
                    process::exit(1);
                }
                dev_size_mb = args[i].parse().unwrap_or(64);
            }
            "-h" | "--help" => {
                print_usage(program);
                process::exit(0);
            }
            _ => {
                print_usage(program);
                process::exit(1);
            }
        }
        i += 1;
    }

    let dev_size = dev_size_mb * 1024 * 1024;

    println!("ublk-daemon starting...");
    println!("  Device ID: {}", if dev_id < 0 { "auto".to_string() } else { dev_id.to_string() });
    println!("  Device size: {} MB", dev_size_mb);

    // Check if running as root
    unsafe {
        if libc::getuid() != 0 {
            eprintln!("Warning: Not running as root, ublk operations may fail");
        }
    }

    // Create controller
    let ctrl = libublk::ctrl::UblkCtrlBuilder::default()
        .name("ublk-daemon-ram")
        .id(dev_id)
        .nr_queues(DEFAULT_NR_QUEUES)
        .depth(DEFAULT_QUEUE_DEPTH)
        .io_buf_bytes(DEFAULT_BUF_SIZE)
        .build()?;

    let dev_id_out = ctrl.dev_info().dev_id;
    println!("  Assigned device ID: {}", dev_id_out);

    // Run target
    ctrl.run_target(
        |_dev| Ok(()), // init
        move |qid, dev| queue_fn(qid, dev, dev_size), // queue handler
        |ctrl| {
            println!("Device /dev/ublkb{} ready", ctrl.dev_info().dev_id);
            ctrl.dump();
        }, // post-start
    )?;

    Ok(())
}

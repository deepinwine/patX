//! patX Core - Rust 安全并发中间层
//!
//! 提供:
//! - 任务并行调度
//! - 内存池管理
//! - 数据校验
//! - FFI 接口

pub mod ffi;
pub mod pool;
pub mod validator;
pub mod scheduler;

use std::os::raw::c_char;
use std::ffi::CString;
use parking_lot::RwLock;
use std::sync::Arc;

/// 专利数据结构 (与 C++ 共享)
#[repr(C)]
pub struct Patent {
    pub id: i32,
    pub geke_code: [u8; 16],
    pub application_no: [u8; 32],
    pub title: [u8; 256],
    pub applicant: [u8; 128],
    pub application_date: [u8; 16],
    pub patent_type: [u8; 32],
    pub legal_status: [u8; 32],
    pub inventor: [u8; 256],
    pub classification: [u8; 64],
    pub agent: [u8; 64],
    pub agency: [u8; 128],
    pub publish_no: [u8; 32],
    pub publish_date: [u8; 16],
    pub grant_date: [u8; 16],
    pub expire_date: [u8; 16],
    pub handler: [u8; 32],
    pub notes: [u8; 512],
    pub patent_level: i32,
    pub is_valid: i32,
    pub created_at: [u8; 32],
    pub updated_at: [u8; 32],
}

impl Default for Patent {
    fn default() -> Self {
        Self {
            id: 0,
            geke_code: [0; 16],
            application_no: [0; 32],
            title: [0; 256],
            applicant: [0; 128],
            application_date: [0; 16],
            patent_type: [0; 32],
            legal_status: [0; 32],
            inventor: [0; 256],
            classification: [0; 64],
            agent: [0; 64],
            agency: [0; 128],
            publish_no: [0; 32],
            publish_date: [0; 16],
            grant_date: [0; 16],
            expire_date: [0; 16],
            handler: [0; 32],
            notes: [0; 512],
            patent_level: 0,
            is_valid: 1,
            created_at: [0; 32],
            updated_at: [0; 32],
        }
    }
}

/// 全局运行时状态
struct RuntimeState {
    initialized: bool,
    scheduler: Option<Arc<scheduler::TaskScheduler>>,
}

lazy_static::lazy_static! {
    static ref RUNTIME: RwLock<RuntimeState> = RwLock::new(RuntimeState {
        initialized: false,
        scheduler: None,
    });
}

/// 系统初始化
#[no_mangle]
pub extern "C" fn patx_init() -> i32 {
    let mut runtime = RUNTIME.write();
    
    if runtime.initialized {
        println!("[patX] Already initialized");
        return 0;
    }
    
    // 初始化任务调度器
    let config = scheduler::ThreadPoolConfig {
        num_threads: num_cpus::get().max(2),
        queue_capacity: 1024,
        thread_name: "patx-worker".to_string(),
    };
    
    runtime.scheduler = Some(Arc::new(scheduler::TaskScheduler::new(config)));
    runtime.initialized = true;
    
    println!("[patX] System initialized with {} workers", config.num_threads);
    0
}

/// 系统关闭
#[no_mangle]
pub extern "C" fn patx_shutdown() -> i32 {
    let mut runtime = RUNTIME.write();
    
    if !runtime.initialized {
        println!("[patX] Not initialized");
        return 0;
    }
    
    // 停止调度器
    if let Some(scheduler) = runtime.scheduler.take() {
        scheduler.stop();
    }
    
    runtime.initialized = false;
    println!("[patX] System shutdown");
    0
}

/// 获取版本
#[no_mangle]
pub extern "C" fn patx_version() -> *mut c_char {
    unsafe { 
        CString::new("0.1.0")
            .unwrap()
            .into_raw() 
    }
}

/// 获取调度器统计
#[no_mangle]
pub extern "C" fn patx_get_stats() -> SchedulerStats {
    let runtime = RUNTIME.read();
    
    if let Some(ref scheduler) = runtime.scheduler {
        let stats = scheduler.stats();
        SchedulerStats {
            total_tasks: stats.total_tasks.load(std::sync::atomic::Ordering::Relaxed),
            pending_tasks: stats.pending_tasks.load(std::sync::atomic::Ordering::Relaxed),
            running_tasks: stats.running_tasks.load(std::sync::atomic::Ordering::Relaxed),
            completed_tasks: stats.completed_tasks.load(std::sync::atomic::Ordering::Relaxed),
            failed_tasks: stats.failed_tasks.load(std::sync::atomic::Ordering::Relaxed),
        }
    } else {
        SchedulerStats::default()
    }
}

/// 调度器统计信息 (C FFI)
#[repr(C)]
#[derive(Default)]
pub struct SchedulerStats {
    pub total_tasks: usize,
    pub pending_tasks: usize,
    pub running_tasks: usize,
    pub completed_tasks: usize,
    pub failed_tasks: usize,
}

/// 提交导入任务
#[no_mangle]
pub extern "C" fn patx_submit_import(file_path: *const c_char, format: i32) -> u64 {
    let runtime = RUNTIME.read();
    
    if let Some(ref scheduler) = runtime.scheduler {
        let path = ffi::c_str_to_string(file_path);
        let fmt = match format {
            0 => scheduler::ImportFormat::Excel,
            1 => scheduler::ImportFormat::Csv,
            2 => scheduler::ImportFormat::Json,
            _ => scheduler::ImportFormat::Excel,
        };
        
        let task = scheduler::Task::new(scheduler::TaskType::Import {
            file_path: path,
            format: fmt,
        });
        
        scheduler.submit(task).unwrap_or(0)
    } else {
        0
    }
}

/// 提交导出任务
#[no_mangle]
pub extern "C" fn patx_submit_export(file_path: *const c_char, format: i32) -> u64 {
    let runtime = RUNTIME.read();
    
    if let Some(ref scheduler) = runtime.scheduler {
        let path = ffi::c_str_to_string(file_path);
        let fmt = match format {
            0 => scheduler::ExportFormat::Excel,
            1 => scheduler::ExportFormat::Csv,
            2 => scheduler::ExportFormat::Json,
            _ => scheduler::ExportFormat::Excel,
        };
        
        let task = scheduler::Task::new(scheduler::TaskType::Export {
            file_path: path,
            format: fmt,
            filter: None,
        });
        
        scheduler.submit(task).unwrap_or(0)
    } else {
        0
    }
}

/// 提交搜索任务
#[no_mangle]
pub extern "C" fn patx_submit_search(query: *const c_char, limit: usize) -> u64 {
    let runtime = RUNTIME.read();
    
    if let Some(ref scheduler) = runtime.scheduler {
        let q = ffi::c_str_to_string(query);
        
        let task = scheduler::Task::new(scheduler::TaskType::Search {
            query: q,
            limit,
        });
        
        scheduler.submit(task).unwrap_or(0)
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_init() {
        assert_eq!(patx_init(), 0);
        assert_eq!(patx_shutdown(), 0);
    }

    #[test]
    fn test_version() {
        let version = patx_version();
        assert!(!version.is_null());
        
        let v = unsafe { CString::from_raw(version) };
        assert_eq!(v.to_str().unwrap(), "0.1.0");
    }

    #[test]
    fn test_patent_default() {
        let p = Patent::default();
        assert_eq!(p.id, 0);
        assert_eq!(p.is_valid, 1);
    }
}

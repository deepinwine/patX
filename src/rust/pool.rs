//! 内存池管理模块
//! 
//! 提供高性能的对象池和缓冲区池

use std::alloc::{alloc, dealloc, Layout};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::mem;

/// 对象池 - 预分配内存避免频繁申请释放
pub struct ObjectPool<T> {
    pool: Vec<NonNull<T>>,
    allocated: AtomicUsize,
    capacity: usize,
}

impl<T> ObjectPool<T> {
    /// 创建新的对象池
    pub fn new(capacity: usize) -> Self {
        let mut pool = Vec::with_capacity(capacity);
        
        for _ in 0..capacity {
            let layout = Layout::new::<T>();
            unsafe {
                let ptr = alloc(layout) as *mut T;
                if let Some(nn) = NonNull::new(ptr) {
                    pool.push(nn);
                }
            }
        }
        
        Self {
            pool,
            allocated: AtomicUsize::new(0),
            capacity,
        }
    }

    /// 从池中获取一个对象
    pub fn acquire(&mut self) -> Option<NonNull<T>> {
        if self.pool.is_empty() {
            None
        } else {
            self.allocated.fetch_add(1, Ordering::Relaxed);
            self.pool.pop()
        }
    }

    /// 将对象归还到池中
    pub fn release(&mut self, ptr: NonNull<T>) {
        self.pool.push(ptr);
        self.allocated.fetch_sub(1, Ordering::Relaxed);
    }

    /// 获取池统计信息
    pub fn stats(&self) -> PoolStats {
        PoolStats {
            capacity: self.capacity,
            available: self.pool.len(),
            allocated: self.allocated.load(Ordering::Relaxed),
        }
    }

    /// 获取可用数量
    pub fn available(&self) -> usize {
        self.pool.len()
    }

    /// 获取已分配数量
    pub fn allocated(&self) -> usize {
        self.allocated.load(Ordering::Relaxed)
    }
}

impl<T> Drop for ObjectPool<T> {
    fn drop(&mut self) {
        // 释放所有预分配的内存
        let layout = Layout::new::<T>();
        for ptr in &self.pool {
            unsafe {
                dealloc(ptr.as_ptr() as *mut u8, layout);
            }
        }
    }
}

// 确保ObjectPool可以安全地在线程间共享
unsafe impl<T: Send> Send for ObjectPool<T> {}
unsafe impl<T: Sync> Sync for ObjectPool<T> {}

/// 池统计信息
#[derive(Debug, Clone, Copy)]
pub struct PoolStats {
    pub capacity: usize,
    pub available: usize,
    pub allocated: usize,
}

/// 缓冲区池 - 用于字符串和大块数据
pub struct BufferPool {
    buffers: Vec<Vec<u8>>,
    buffer_size: usize,
    capacity: usize,
}

impl BufferPool {
    /// 创建新的缓冲区池
    pub fn new(buffer_size: usize, capacity: usize) -> Self {
        let mut buffers = Vec::with_capacity(capacity);
        for _ in 0..capacity {
            buffers.push(vec![0u8; buffer_size]);
        }
        
        Self {
            buffers,
            buffer_size,
            capacity,
        }
    }

    /// 获取缓冲区
    pub fn acquire(&mut self) -> Option<Vec<u8>> {
        if self.buffers.is_empty() {
            // 池耗尽时创建新缓冲区
            Some(vec![0u8; self.buffer_size])
        } else {
            let mut buf = self.buffers.pop()?;
            // 清零
            for b in &mut buf {
                *b = 0;
            }
            Some(buf)
        }
    }

    /// 归还缓冲区
    pub fn release(&mut self, buf: Vec<u8>) {
        if self.buffers.len() < self.capacity && buf.len() == self.buffer_size {
            self.buffers.push(buf);
        }
        // 否则丢弃
    }

    /// 获取可用缓冲区数量
    pub fn available(&self) -> usize {
        self.buffers.len()
    }
}

/// 专利数据结构 (与 C++ 共享)
#[repr(C)]
#[derive(Clone, Copy, Default)]
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

impl Patent {
    /// 创建新专利实例
    pub fn new() -> Self {
        Self::default()
    }

    /// 设置字符串字段
    pub fn set_str(&mut self, field: &mut [u8], value: &str) {
        let bytes = value.as_bytes();
        let len = bytes.len().min(field.len() - 1);
        field[..len].copy_from_slice(&bytes[..len]);
        field[len] = 0;
    }

    /// 获取字符串字段
    pub fn get_str(field: &[u8]) -> &str {
        let end = field.iter().position(|&b| b == 0).unwrap_or(field.len());
        std::str::from_utf8(&field[..end]).unwrap_or("")
    }
}

lazy_static::lazy_static! {
    /// 全局专利内存池
    static ref PATENT_POOL: Mutex<ObjectPool<Patent>> = 
        Mutex::new(ObjectPool::new(10000));
    
    /// 全局缓冲区池 (4KB 缓冲区)
    static ref BUFFER_POOL: Mutex<BufferPool> =
        Mutex::new(BufferPool::new(4096, 100));
}

/// 从全局池获取专利对象
pub fn acquire_patent() -> Option<Patent> {
    let mut pool = PATENT_POOL.lock().ok()?;
    let ptr = pool.acquire()?;
    let patent = unsafe { ptr.as_ptr().read() };
    pool.release(ptr);
    Some(patent)
}

/// 将专利对象归还到全局池
pub fn release_patent(patent: Patent) {
    if let Ok(mut pool) = PATENT_POOL.lock() {
        let layout = std::alloc::Layout::new::<Patent>();
        unsafe {
            let ptr = std::alloc::alloc(layout) as *mut Patent;
            if !ptr.is_null() {
                ptr.write(patent);
                if let Some(nn) = NonNull::new(ptr) {
                    pool.release(nn);
                }
            }
        }
    }
}

/// 获取全局池统计
pub fn get_pool_stats() -> PoolStats {
    PATENT_POOL.lock()
        .map(|p| p.stats())
        .unwrap_or(PoolStats {
            capacity: 0,
            available: 0,
            allocated: 0,
        })
}

/// 从缓冲区池获取缓冲区
pub fn acquire_buffer() -> Option<Vec<u8>> {
    BUFFER_POOL.lock().ok()?.acquire()
}

/// 归还缓冲区
pub fn release_buffer(buf: Vec<u8>) {
    if let Ok(mut pool) = BUFFER_POOL.lock() {
        pool.release(buf);
    }
}

/// 内存管理器 - 统一管理各种池
pub struct MemoryManager {
    patent_pool_capacity: usize,
    buffer_size: usize,
    buffer_pool_capacity: usize,
}

impl MemoryManager {
    /// 创建新的内存管理器
    pub fn new() -> Self {
        Self {
            patent_pool_capacity: 10000,
            buffer_size: 4096,
            buffer_pool_capacity: 100,
        }
    }

    /// 设置专利池容量
    pub fn with_patent_pool_capacity(mut self, capacity: usize) -> Self {
        self.patent_pool_capacity = capacity;
        self
    }

    /// 设置缓冲区大小
    pub fn with_buffer_size(mut self, size: usize) -> Self {
        self.buffer_size = size;
        self
    }

    /// 设置缓冲区池容量
    pub fn with_buffer_pool_capacity(mut self, capacity: usize) -> Self {
        self.buffer_pool_capacity = capacity;
        self
    }

    /// 初始化内存管理器
    pub fn init(&self) {
        // 池已在 lazy_static 中初始化
        println!("[patX] Memory manager initialized");
        println!("  Patent pool capacity: {}", self.patent_pool_capacity);
        println!("  Buffer size: {} bytes", self.buffer_size);
        println!("  Buffer pool capacity: {}", self.buffer_pool_capacity);
    }

    /// 获取统计信息
    pub fn stats(&self) -> MemoryStats {
        let pool_stats = get_pool_stats();
        let buffer_available = BUFFER_POOL.lock()
            .map(|p| p.available())
            .unwrap_or(0);
        
        MemoryStats {
            patent_pool: pool_stats,
            buffer_pool_available: buffer_available,
            buffer_pool_capacity: self.buffer_pool_capacity,
        }
    }
}

impl Default for MemoryManager {
    fn default() -> Self {
        Self::new()
    }
}

/// 内存统计信息
#[derive(Debug, Clone)]
pub struct MemoryStats {
    pub patent_pool: PoolStats,
    pub buffer_pool_available: usize,
    pub buffer_pool_capacity: usize,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_object_pool() {
        let mut pool: ObjectPool<i32> = ObjectPool::new(10);
        
        let stats = pool.stats();
        assert_eq!(stats.capacity, 10);
        assert_eq!(stats.available, 10);
        assert_eq!(stats.allocated, 0);
        
        let ptr = pool.acquire();
        assert!(ptr.is_some());
        
        let stats = pool.stats();
        assert_eq!(stats.available, 9);
        assert_eq!(stats.allocated, 1);
        
        if let Some(p) = ptr {
            pool.release(p);
        }
        
        let stats = pool.stats();
        assert_eq!(stats.available, 10);
    }

    #[test]
    fn test_buffer_pool() {
        let mut pool = BufferPool::new(1024, 5);
        assert_eq!(pool.available(), 5);
        
        let buf = pool.acquire();
        assert!(buf.is_some());
        assert_eq!(buf.unwrap().len(), 1024);
        
        assert_eq!(pool.available(), 4);
    }

    #[test]
    fn test_patent() {
        let mut patent = Patent::new();
        patent.id = 1;
        patent.set_str(&mut patent.geke_code, "GC-0001");
        patent.set_str(&mut patent.title, "测试专利");
        
        assert_eq!(patent.id, 1);
        assert_eq!(Patent::get_str(&patent.geke_code), "GC-0001");
        assert_eq!(Patent::get_str(&patent.title), "测试专利");
    }
}

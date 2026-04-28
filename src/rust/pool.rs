//! 内存池管理模块

use std::alloc::{alloc, dealloc, Layout};
use std::ptr::NonNull;
use std::sync::atomic::{AtomicUsize, Ordering};

/// 对象池统计
#[derive(Debug, Clone, Copy)]
pub struct PoolStats {
    pub capacity: usize,
    pub available: usize,
    pub allocated: usize,
}

/// 对象池
pub struct ObjectPool<T> {
    pool: Vec<NonNull<T>>,
    allocated: AtomicUsize,
    capacity: usize,
}

impl<T> ObjectPool<T> {
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

    pub fn acquire(&mut self) -> Option<NonNull<T>> {
        if self.pool.is_empty() {
            None
        } else {
            self.allocated.fetch_add(1, Ordering::Relaxed);
            self.pool.pop()
        }
    }

    pub fn release(&mut self, ptr: NonNull<T>) {
        self.pool.push(ptr);
        self.allocated.fetch_sub(1, Ordering::Relaxed);
    }

    pub fn stats(&self) -> PoolStats {
        PoolStats {
            capacity: self.capacity,
            available: self.pool.len(),
            allocated: self.allocated.load(Ordering::Relaxed),
        }
    }
}

impl<T> Drop for ObjectPool<T> {
    fn drop(&mut self) {
        let layout = Layout::new::<T>();
        for ptr in &self.pool {
            unsafe {
                dealloc(ptr.as_ptr() as *mut u8, layout);
            }
        }
    }
}

unsafe impl<T: Send> Send for ObjectPool<T> {}
unsafe impl<T: Sync> Sync for ObjectPool<T> {}

/// 缓冲区池
pub struct BufferPool {
    buffers: Vec<Vec<u8>>,
    buffer_size: usize,
    capacity: usize,
}

impl BufferPool {
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

    pub fn acquire(&mut self) -> Option<Vec<u8>> {
        if self.buffers.is_empty() {
            Some(vec![0u8; self.buffer_size])
        } else {
            let mut buf = self.buffers.pop()?;
            for b in &mut buf {
                *b = 0;
            }
            Some(buf)
        }
    }

    pub fn release(&mut self, buf: Vec<u8>) {
        if self.buffers.len() < self.capacity && buf.len() == self.buffer_size {
            self.buffers.push(buf);
        }
    }
}

/// 内存管理器
pub struct MemoryManager {
    patent_pool_capacity: usize,
    buffer_size: usize,
    buffer_pool_capacity: usize,
}

impl MemoryManager {
    pub fn new() -> Self {
        Self {
            patent_pool_capacity: 10000,
            buffer_size: 4096,
            buffer_pool_capacity: 100,
        }
    }

    pub fn init(&self) {
        println!("[patX] Memory manager initialized");
    }
}

impl Default for MemoryManager {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_object_pool() {
        let mut pool: ObjectPool<i32> = ObjectPool::new(10);
        let stats = pool.stats();
        assert_eq!(stats.capacity, 10);
    }

    #[test]
    fn test_buffer_pool() {
        let mut pool = BufferPool::new(1024, 5);
        assert_eq!(pool.buffers.len(), 5);
        
        let buf = pool.acquire();
        assert!(buf.is_some());
    }
}
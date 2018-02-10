#include <sruntime.h>
#include <system.h>

// allocator
unsigned int process_count = 110;

typedef struct file {
    u64 offset; 
    io read, write;
    node n;
} *file;

#define NUMBER_OF_FDS 32
typedef struct process {
    heap h, pages, physical;
    int pid;
    node filesystem;
    // could resize
    struct file files[NUMBER_OF_FDS];
    void *brk;
    heap virtual;
    heap virtual32;    
    heap fdallocator;
    node cwd;
} *process;

thread current;

// could really take the args directly off the function..maybe dispatch in
// asm
// packed?

static node lookup(process p, char *name)
{
    struct buffer b;
    b.start = 0;
    b.end = runtime_strlen(name);
    b.contents = name;
    // transient
    vector vn = split(p->h, &b, '/');
    vector_pop(vn);
    // relative
    if (vector_length(vn) == 0) {
        return node_invalid;
    }
    
    return storage_resolve(p->filesystem, vn);
}

int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact)
{
    rprintf("sigacts %d\n", signum);
    if (oldact) oldact->_u._sa_handler = SIG_DFL;
    return 0;
}



int read(int fd, u8 *dest, bytes length)
{
    file f = current->p->files + fd;
    return apply(f->read, dest, length, f->offset);
}

// callibration is an issue
int gettimeofday(struct timeval *tv, void *tz)
{
    static u64 seconds;
    static u64 microseconds;
    tv->tv_sec = seconds;
    tv->tv_usec = microseconds++;
    return 0;
}

int write(int fd, u8 *body, bytes length)
{
    file f = current->p->files +fd;
    int res = apply(f->write, body, length, f->offset);
    f->offset += length;
    return res;
}

static int writev(int fd, iovec v, int count)
{
    int res;
    file f = current->p->files +fd;    
    for (int i = 0; i < count; i++) res += write(fd, v[i].address, v[i].length);
    return res;
}

static int access(char *name, int mode)
{
    void *where;
    bytes length;
    rprintf("access %s\n", name);    
    if (is_empty(lookup(current->p, name)))
        return -ENOENT;
    return 0;
}

static CLOSURE_1_3(contents_read, int, node, void *, u64, u64);
static int contents_read(node n, void *dest, u64 length, u64 offset)
{
    void *base;
    u64 flength;
    rprintf("contents read %p %p %p %p\n", base, length, offset, flength);
    if (!node_contents(n, &base, &flength)) return -EINVAL;
    rprintf("contents read %p %p %p %p\n", base, length, offset, flength);
    if (length < flength) {
        flength = length;
    }
    rprintf("contents read %p %p %p %p %p\n", base, length, offset, flength, *(u64 *)(base + offset));
    runtime_memcpy(dest, base + offset, flength);
    return flength;
}

long clone(unsigned long flags, void *child_stack, void *ptid, void *ctid, void *x)
{
    static int tid= 2;
    rprintf("clone! %p %p %p %p %p\n", flags, child_stack, ptid, ctid, x);
    thread t = create_thread(current->p);
    runtime_memcpy(t->frame, current->frame, sizeof(t->frame));
    t->frame[FRAME_RSP]= u64_from_pointer(child_stack);
    t->frame[FRAME_RAX]= *(u32 *)ctid;
    // run should do this!
    write_msr(FS_MSR, u64_from_pointer(x));
    rprintf("fs: %p\n", read_msr(FS_MSR));
    return 0;
    //    run(t);
}


int open(char *name, int flags, int mode)
{
    struct node n;
    bytes length;
    rprintf("open %s\n", name);
    
    // fix - lookup should be robust
    if (name == 0) return -EINVAL;
    
    if (is_empty(n = lookup(current->p, name)))
        return -ENOENT;

    buffer b = allocate(current->p->h, sizeof(struct buffer));
    // might be functional, or be a directory
    int fd = allocate_u64(current->p->fdallocator, 1);
    file f = current->p->files+fd;
    f->n = n;
    f->read = closure(current->p->h, contents_read, n);
    f->offset = 0;
    rprintf("open return %x\n", fd);
    return fd;
}

#ifndef MIN
#define MIN(x, y) ((x) < (y)? (x):(y))
#endif

void *mremap(void *old_address, u64 old_size,  u64 new_size, int flags,  void *new_address )
{
    // this seems poorly thought out - what if there is a backing file?
    // and if its anonymous why do we care where it is..i guess this
    // is just for large realloc operations? if these aren't aligned
    // its completely unclear what to do
    rprintf ("mremap %p %x %x %x %p\n", old_address, old_size, new_size, flags, new_address);
    u64 align =  ~MASK(PAGELOG);
    if (new_size > old_size) {
        u64 diff = pad(new_size - old_size, PAGESIZE);
        u64 base = u64_from_pointer(old_address + old_size) & align;
        void *r = allocate(current->p->physical,diff);
        if (u64_from_pointer(r) == PHYSICAL_INVALID) {
            // MAP_FAILED
            return r;
        }
        rprintf ("new alloc %p %x\n", r, diff);
        map(base, physical_from_virtual(r), diff, current->p->pages);
        zero(pointer_from_u64(base), diff); 
    }
    //    map(u64_from_pointer(new_address)&align, physical_from_virtual(old_address), old_size, current->p->pages);
    return old_address;
}


static void *mmap(void *target, u64 size, int prot, int flags, int fd, u64 offset)
{
    rprintf("mmap %p %p %x %d %p\n", target, size, flags, fd, offset);
    process p = current->p;
    // its really unclear whether this should be extended or truncated
    u64 len = pad(size, PAGESIZE);
    //gack
    len = len & MASK(32);
    u64 where = u64_from_pointer(target);

    if (!(flags &MAP_FIXED)){
        if (flags & MAP_32BIT)
            where = allocate_u64(current->p->virtual32, len);
        else
            where = allocate_u64(current->p->virtual, len);
    }
        
    // make a generic zero page function
    if (flags & MAP_ANONYMOUS) {
        u64  m = allocate_u64(p->physical, len);
        if (m == PHYSICAL_INVALID) return pointer_from_u64(m);
        map(where, m, len, p->pages);
        zero(pointer_from_u64(where), len);
        return pointer_from_u64(where);
    }
    

    // check that fd is valid
    file f = p->files + fd;
    void *fbase;
    u64 flen;
    if (!node_contents(f->n, &fbase, &flen)) return pointer_from_u64(PHYSICAL_INVALID);

    u64 msize = 0;
    if (flen > offset) msize = pad(flen-offset, PAGESIZE);
    if (msize > len) msize = len;
    
    // mutal misalignment?...discontiguous backing?
    map(where, physical_from_virtual(fbase + offset), msize, p->pages);

    if (len > msize) {
        u64 bss = pad(len, PAGESIZE) - msize;
        map(where + msize, allocate_u64(p->physical, bss), bss, p->pages);
        rprintf("zero: %p %x\n", pointer_from_u64(where+msize), bss);
        zero(pointer_from_u64(where+msize), bss);
    }
    // ok, if we change pages entries we need to flush the tlb...dont need
    // to do this every time
    u64 x;
    mov_from_cr("cr3", x);
    mov_to_cr("cr3", x);    
    return pointer_from_u64(where);
}

static boolean fill_stat(node n, struct stat *s)
{
    void *fbase;
    u64 flen;
    
    s->st_dev = 0;
    s->st_ino = u64_from_pointer(n.offset);
    // dir doesn't have contents
    if (!node_contents(n, &fbase, &flen)) return false;    
    s->st_size = flen;

    if (flen == 0) {
        // fix dir demux
        s->st_mode = S_IFDIR | 0777;
    }
}

static int fstat(int fd, struct stat *s)
{
    // take this from tuple space
    if (fd == 1) {
        s->st_mode = S_IFIFO;
        return 0;
    }
    fill_stat(current->p->files[fd].n, s);
    return 0;
}

static int futex(int *uaddr, int futex_op, int val,
                 const struct timespec *timeout,   
                 int *uaddr2, int val3)
{
    rprintf("futex op %d %x %d %p %p\n", futex_op, uaddr, val, current->frame[FRAME_RDX], timeout);
#if 0
    u64 *stack = pointer_from_u64(current->frame[FRAME_RSP]);        
    for (int j = 0; j< 20; j++) {
        print_u64(stack[j]);
        console("\n");        
    }
    asm("hlt");
#endif        
    int op = futex_op & 127; // chuck the private bit
    switch(op) {
    case FUTEX_WAIT: rprintf("futex_wait\n"); {
            //       *uaddr = val;
            return 0;
        }
    case FUTEX_WAKE: rprintf("futex_wake\n"); break;
    case FUTEX_FD: rprintf("futex_fd\n"); break;
    case FUTEX_REQUEUE: rprintf("futex_requeue\n"); break;
    case FUTEX_CMP_REQUEUE: rprintf("FUTEX_cmp_requeue\n"); break;
    case FUTEX_WAKE_OP: rprintf("FUTEX_wake_op\n"); break;
    case FUTEX_WAIT_BITSET: rprintf("FUTEX_wait_bitset\n"); break;
    case FUTEX_WAKE_BITSET: rprintf("FUTEX_wake_bitset\n"); break;
    case FUTEX_LOCK_PI: rprintf("FUTEX_lock_pi\n"); break;
    case FUTEX_TRYLOCK_PI: rprintf("FUTEX_trylock_pi\n"); break;
    case FUTEX_UNLOCK_PI: rprintf("FUTEX_unlock_pi\n"); break;
    case FUTEX_CMP_REQUEUE_PI: rprintf("FUTEX_CMP_requeue_pi\n"); break;
    case FUTEX_WAIT_REQUEUE_PI: rprintf("FUTEX_WAIT_requeue_pi\n"); break;
    }
}


static int stat(char *name, struct stat *s)
{
    u64 where = 0;
    bytes length;
    node n;

    rprintf("stat %s\n", name);
    if (is_empty(n = lookup(current->p, name)))
        return -ENOENT;

    fill_stat(n, s);
    return 0;
}

static u64 lseek(int fd, u64 offset, int whence)
{
    rprintf("lseek %d %p %d\n", fd, offset, whence);
    return current->p->files[fd].offset;
}


extern void write_msr(u64 a, u64 b);
static int arch_prctl(int code, unsigned long a)
{
    rprintf("arch prctl op %x\n", code);
    switch (code) {
    case ARCH_SET_GS:
        break;
    case ARCH_SET_FS:
        write_msr(FS_MSR, a);
        return 0;
        break;
    case ARCH_GET_FS:
        break;
    case ARCH_GET_GS:
        break;
    default:
        return -EINVAL;
    }
}

static int uname(struct utsname *v)
{
    char rel[]= "4.4.0-87";
    char sys[] = "pugnix";
    runtime_memcpy(v->sysname,sys, sizeof(sys));
    runtime_memcpy(v->release, rel, sizeof(rel));
    return 0;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    switch (resource) {
    case RLIMIT_STACK:
        rlim->rlim_cur = 2*1024*1024;
        rlim->rlim_max = 2*1024*1024;
        return 0;
    case RLIMIT_NOFILE:
        rlim->rlim_cur = NUMBER_OF_FDS;
        rlim->rlim_max = NUMBER_OF_FDS;
        return 0;
    }
    return -1;
}

static void *brk(void *x)
{
    process p = current->p;
    // stash end of bss?
    if (p->brk) {
        if (p->brk > x) {
            p->brk = x;
            // free
        } else {
            u64 alloc = u64_from_pointer(x) - u64_from_pointer(p->brk);
            map(u64_from_pointer(p->brk), allocate_u64(p->physical, alloc), alloc, p->pages);
            zero(p->brk, alloc);
            p->brk += alloc;         
        }
    }
    return p->brk;
}

u64 readlink(const char *pathname, char *buf, size_t bufsiz)
{
    rprintf("readlink %s\n", pathname);
    return -EINVAL;

}


// because the conventions mostly line up, and because the lower level
// handler doesn't touch these, using the arglist here should be
// a bit faster than digging them out of frame
// need to change to deal with errno conventions
u64 syscall()
{
    u64 *f = current->frame;
    int call = f[FRAME_VECTOR];
    u64 a[6] = {f[FRAME_RDI], f[FRAME_RSI], f[FRAME_RDX], f[FRAME_R10], f[FRAME_R8], f[FRAME_R9]};
    // vector dispatch with things like fd decoding and general error processing
    if ((call != SYS_write) && (call != SYS_gettimeofday) && (call != SYS_writev) && (call != SYS_getpid))
        rprintf("syscall %d %p %p %p\n", call, a[0], a[1], a[2]);
    switch (call) {
    case SYS_read: return read(a[0],pointer_from_u64(a[1]), a[2]);
    case SYS_write: return write(a[0], pointer_from_u64(a[1]), a[2]);
    case SYS_open: return open(pointer_from_u64(a[0]), a[1], a[2]);
    case SYS_fstat: return fstat(a[0], pointer_from_u64(a[1]));
    case SYS_stat: return stat(pointer_from_u64(a[0]), pointer_from_u64(a[1]));        
    case SYS_writev: return writev(a[0], pointer_from_u64(a[1]), a[2]);
    case SYS_brk: return u64_from_pointer(brk(pointer_from_u64(a[0])));
    case SYS_uname: return uname(pointer_from_u64(a[0]));
    case SYS_mmap: return u64_from_pointer(mmap(pointer_from_u64(a[0]), a[1], a[2], a[3], a[4], a[5]));
    case SYS_access: return access(pointer_from_u64(a[0]), a[1]);
    case SYS_getrlimit: return getrlimit(a[0], pointer_from_u64(a[1]));
    case SYS_getpid: return current->p->pid;
    case SYS_arch_prctl: return arch_prctl(a[0], a[1]);
    case SYS_rt_sigaction: return sigaction(a[0], pointer_from_u64(a[1]), pointer_from_u64(a[2]));        
    case SYS_lseek: return lseek(a[0], a[1], a[2]);        
    case SYS_mremap: return u64_from_pointer(mremap(pointer_from_u64(a[0]), a[1], a[2], a[3], pointer_from_u64(a[4])));        
    case SYS_futex: return futex(pointer_from_u64(a[0]), a[1], a[2], pointer_from_u64(a[3]),
                                 pointer_from_u64(a[4]),a[5]);
    case SYS_readlink: return readlink(pointer_from_u64(a[0]), pointer_from_u64(a[2]), a[3]);
    case SYS_gettimeofday: return gettimeofday(pointer_from_u64(a[0]), pointer_from_u64(a[2]));
    case SYS_clone: return clone(a[0], pointer_from_u64(a[1]), pointer_from_u64(a[2]), pointer_from_u64(a[3]), pointer_from_u64(a[4]));

    default:
        rprintf("syscall %d %p %p %p\n", call, a[0], a[1], a[2]);
        return (0);
    }
}

extern u64 *frame;
void run(thread t)
{
    current = t;
    frame = t->frame;
    void *entry = pointer_from_u64(frame[FRAME_RIP]);
    void *stack = pointer_from_u64(frame[FRAME_RSP]);    
    //  need to use frame_return (full context), but it faults. make a jmp based frame return?
    __asm__("mov %0, %%rax"::"g"(frame[FRAME_RAX]));    
    __asm__("mov %0, %%rsi"::"g"(frame[FRAME_RSI]));
    __asm__("mov %0, %%rdi"::"g"(frame[FRAME_RDI]));
    __asm__("mov %0, %%rdx"::"g"(frame[FRAME_RDX]));
    __asm__("mov %0, %%rcx"::"g"(frame[FRAME_RCX]));
    __asm__("mov %0, %%r8"::"g"(frame[FRAME_R8]));
    __asm__("mov %0, %%r9"::"g"(frame[FRAME_R9]));                        
    __asm__("mov %0, %%rsp"::"g"(stack));
    __asm__("jmp *%0"::"g"(entry));
}

thread create_thread(process p)
{
    thread t = allocate(p->h, sizeof(struct thread));
    t->p = p;
    // stack goes here
    return t;
}

static CLOSURE_0_3(stdout, int, void*, u64, u64);
static int stdout(void *d, u64 length, u64 offset)
{
    character *z = d;
    for (int i = 0; i< length; i++) {
        serial_out(z[i]);
    }
}

process create_process(heap h, heap pages, heap physical, node filesystem)
{
    process p = allocate(h, sizeof(struct process));
    p->filesystem = filesystem;
    p->h = h;
    p->brk = pointer_from_u64(0x8000000);
    p->pid = process_count++;
    // allocate main thread, setup context, run main thread
    p->virtual = create_id_heap(h, 0x7000000000ull, 0x100000000);
    p->virtual32 = create_id_heap(h, 0xd0000000, PAGESIZE);
    p->pages = pages;
    p->fdallocator = create_id_heap(h, 3, 1);
    p->physical = physical;
    p->files[1].write = closure(h, stdout);    
    p->files[2].write = closure(h, stdout);
    return p;
}
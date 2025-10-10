#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "include/userprog/process.h"
#include "include/filesys/file.h"
#include "intrinsic.h"

void syscall_entry(void);
void syscall_handler(struct intr_frame *);
static bool sys_create(const char *file, unsigned initial_size);
unsigned tell(int fd);
bool sys_remove(const char *file);
struct lock filesys_lock;

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void syscall_init(void) {
  write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48 | ((uint64_t)SEL_KCSEG) << 32);
  write_msr(MSR_LSTAR, (uint64_t)syscall_entry);

  /* The interrupt service rountine should not serve any interrupts
   * until the syscall_entry swaps the userland stack to the kernel
   * mode stack. Therefore, we masked the FLAG_FL. */
  write_msr(MSR_SYSCALL_MASK, FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

  lock_init(&filesys_lock);
}

/* The main system call interface */
void syscall_handler(struct intr_frame *f UNUSED) {
  // TODO: Your implementation goes here.
  int syscall = f->R.rax;
  switch (syscall) {
    case SYS_HALT:
      halt();
      break;
    case SYS_EXIT:
      int status = f->R.rdi;
      exit(status);
      break;
    case SYS_FORK:
      tid_t child_tid = process_fork((const char *)f->R.rdi, f);
      f->R.rax = child_tid;
      break;
    case SYS_EXEC:
      f->R.rax = exec(f->R.rdi);
      break;
    case SYS_WAIT:
      f->R.rax = process_wait((tid_t)f->R.rdi);
      break;
    case SYS_CREATE:
      f->R.rax = sys_create((const char *)f->R.rdi, (unsigned)f->R.rsi);
      break;
    case SYS_REMOVE:
      f->R.rax = sys_remove((const char *)f->R.rdi);
      break;
    case SYS_OPEN:
      f->R.rax = open((const char *)f->R.rdi);
      break;
    case SYS_FILESIZE:
      f->R.rax = filesize(f->R.rdi);
      break;
    case SYS_READ:
      f->R.rax = read((int)f->R.rdi, f->R.rsi, (unsigned int)f->R.rdx);
      break;
    case SYS_WRITE:
      f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
      break;
    case SYS_SEEK:
      seek((int)f->R.rdi, (unsigned)f->R.rsi);
      break;
    case SYS_TELL:
      f->R.rax = tell(f->R.rdi);
      break;
    case SYS_CLOSE:
      close((int)f->R.rdi);
      break;
    default:
      printf("Unknown syscall: %d\n", (int)f->R.rax);
      break;
  }
  // printf ("system call!\n");
  // thread_exit ();
}

int write(int fd, const void *buffer, unsigned size) {
  get_safe_buffer(buffer, size);

  if (fd < 0 || fd >= FDT_SIZE) {
    return -1;
  }

  if (fd == 0) {  // STDIN
    return -1;
  } else if (fd == 1) {  // STDOUT
    putbuf(buffer, size);
    return size;
  } else {
    struct thread *t = thread_current();
    struct file *target_file = t->fdt[fd];

    if (target_file == NULL) {
      return -1;
    }

    int bytes_written = file_write(target_file, buffer, size);

    return bytes_written;
  }
}

int exec(void *f_name) {
  if (f_name == NULL || !is_user_vaddr(f_name)) {
    return -1;
  }
  void *ptr = pg_round_down(f_name);  //페이지의 초깃값
  if (spt_find_page(&thread_current()->spt, ptr) == NULL) {
    exit(-1);
  }

  char file_name[128];
  memcpy(file_name, f_name, sizeof(file_name));

  int num = process_exec(file_name);

  return num;
}

void exit(int status) {
  struct thread *cur = thread_current();
  cur->exit_status = status;
  printf("%s: exit(%d)\n", cur->name, status);
  thread_exit();
}

void halt() { power_off(); }

static bool sys_create(const char *file, unsigned initial_size) {
  // 1. NULL 포인터이거나 2. 커널 영역 주소이거나 3. 매핑되지 않은 주소이면 종료
  if (file == NULL || !is_user_vaddr(file) || pml4_get_page(thread_current()->pml4, file) == NULL) {
    exit(-1);
  }
  if (strlen(file) == 0 || strlen(file) > NAME_MAX) {  //== 0 equals to "" 빈문자열
    return false;
  }

  bool success = filesys_create(file, initial_size);

  return success;
}

int open(const char *file) {
  if (file == NULL || !is_user_vaddr(file) || pml4_get_page(thread_current()->pml4, file) == NULL) {
    exit(-1);
  }

  lock_acquire(&filesys_lock);

  struct file *file_open = filesys_open(file);
  if (file_open == NULL) {
    lock_release(&filesys_lock);
    return -1;
  }
  int fd = get_fd_from_fdt(file_open);

  if (fd == -1) {
    file_close(file_open);
  }
  lock_release(&filesys_lock);

  return fd;
}

void close(int fd) {
  struct thread *t = thread_current();
  if (fd < 2 || fd >= FDT_SIZE) {
    exit(-1);
  }
  struct file *my_fd_file = t->fdt[fd];

  if (my_fd_file == NULL) {
    exit(-1);
  }

  lock_acquire(&filesys_lock);
  file_close(my_fd_file);
  lock_release(&filesys_lock);

  t->fdt[fd] = NULL;
}

int filesize(int fd) {
  if (fd < 0 || fd >= FDT_SIZE) {
    return -1;
  }
  struct thread *t = thread_current();
  struct file *target_file = t->fdt[fd];

  if (target_file == NULL) {
    return -1;
  }

  lock_acquire(&filesys_lock);
  int size = file_length(target_file);
  lock_release(&filesys_lock);

  return size;
}

int read(int fd, void *buffer, unsigned size) {
  get_safe_buffer(buffer, size);
  // printf("DEBUG fd_read: fd=%d, buffer=%p, size=%u\n", fd, buffer, size);

  if (size == 0) {
    return 0;
  }
  if (fd < 0 || fd >= FDT_SIZE) {
    return -1;
  }
  char *char_buffer = (char *)buffer;
  int bytes_read = 0;
  if (fd == 0) {  // STDIN
    for (int i = 0; i < size; i++) {
      int c = input_getc();

      // if(c == EOF){
      // 	break;
      // }
      char_buffer[i] = (char)c;
      bytes_read++;
    }

    return bytes_read;
  } else if (fd == 1) {  // STDOUT read하지 않음
    return -1;
  } else {
    struct thread *t = thread_current();
    if (t->fdt[fd] == NULL) {
      return -1;
    }

    lock_acquire(&filesys_lock);
    bytes_read = file_read(t->fdt[fd], buffer, size);
    // printf("바이트 리드: %d", bytes_read);
    lock_release(&filesys_lock);

    return bytes_read;
  }
}

void get_safe_buffer(void *buffer, unsigned size) {
  if (buffer == NULL || !is_user_vaddr(buffer)) {
    exit(-1);
  }
  if (!is_user_vaddr(buffer + size - 1)) {
    exit(-1);
  }

#ifdef VM
  void *ptr = pg_round_down(buffer);  //페이지의 초깃값
  void *endptr = buffer + size - 1;
  for (; ptr <= endptr; ptr += PGSIZE) {  //페이지 별로 확인
    if (spt_find_page(&thread_current()->spt, ptr) == NULL) {
      exit(-1);
    }
  }
#else
  void *ptr = pg_round_down(buffer);  //페이지의 초깃값
  void *endptr = buffer + size - 1;
  for (; ptr <= endptr; ptr += PGSIZE) {  //페이지 별로 확인
    if (pml4_get_page(thread_current()->pml4, ptr) == NULL) {
      exit(-1);
    }
  }

#endif
}

void seek(int fd, unsigned position) {
  if (fd < 2 || fd >= FDT_SIZE) {
    return -1;
  }

  struct thread *t = thread_current();

  struct file *targetfile = t->fdt[fd];
  if (targetfile == NULL) {
    exit(-1);
  }
  lock_acquire(&filesys_lock);
  file_seek(targetfile, position);
  lock_release(&filesys_lock);
}

bool sys_remove(const char *file) {
  lock_acquire(&filesys_lock);
  bool success = filesys_remove(file);
  lock_release(&filesys_lock);

  return success;
}

unsigned tell(int fd) {
  if (fd < 2 || fd >= FDT_SIZE) {
    exit(-1);
  }

  struct thread *t = thread_current();

  struct file *targetfile = t->fdt[fd];
  if (targetfile == NULL) {
    exit(-1);
  }
  lock_acquire(&filesys_lock);
  off_t file_tell_return = file_tell(targetfile);
  lock_release(&filesys_lock);

  return file_tell_return;
}

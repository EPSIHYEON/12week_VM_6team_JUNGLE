#include "userprog/syscall.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>

#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "include/filesys/file.h"
#include "include/userprog/process.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#ifdef VM
#include "vm/vm.h"
#endif

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
static void validate_user_buffer(const void *buffer, unsigned size, bool writable);

void syscall_handler(struct intr_frame *f UNUSED) {
  // TODO: Your implementation goes here.
#ifdef VM
  thread_current()->user_rsp = (uint8_t *)f->rsp;
#endif
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
  validate_user_buffer(buffer, size, false);

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
  validate_user_buffer(buffer, size, true);
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

/*
 *   - 유저 포인터 `buffer`가 가리키는 [buffer, buffer+size) 구간이
 *     유저 공간에 있고 접근 가능한지 확인하고, 필요 시 페이지를 클레임한다.
 * 인자:
 *   - buffer: 유저 영역 버퍼 시작 주소(유효한 유저 가상주소여야 함)
 *   - size:   검사할 바이트 크기(0이면 즉시 반환)
 *   - writable: true이면 쓰기 가능한 페이지인지 확인(쓰기 불가면 종료)
 */
static void validate_user_buffer(const void *buffer, unsigned size, bool writable) {
  if (size == 0) return;

  /* 널 포인터는 잘못된 인자이므로 종료 */
  if (buffer == NULL) exit(-1);

  /* 시작 주소를 바이트 포인터로 변환 */
  const uint8_t *start = (const uint8_t *)buffer;
  /* 64비트 정수로 시작 주소 저장 */
  uint64_t start_addr = (uint64_t)start;
  /* 끝 주소 = 시작 + 크기 - 1 (포함 범위) */
  uint64_t end_addr = start_addr + size - 1;

  /* overflow guard: 덧셈 오버플로우로 범위 역전되면 종료 */
  if (end_addr < start_addr) exit(-1);

  /* 전체 범위가 유저영역에 있어야 함 */
  if (!is_user_vaddr((const void *)start_addr) || !is_user_vaddr((const void *)end_addr)) {
    exit(-1);
  }

  /* 페이지 경계로 내린 시작/끝 페이지 주소 */
  uint8_t *page_begin = (uint8_t *)pg_round_down((void *)start_addr);
  uint8_t *page_end = (uint8_t *)pg_round_down((void *)end_addr);

#ifdef VM
  struct thread *curr = thread_current();
  struct supplemental_page_table *spt = &curr->spt;
#endif

  /* page_begin부터 page_end까지 페이지 경계 단위로 순회(한 번에 PGSIZE씩 증가) */
  for (uint8_t *addr = page_begin; addr <= page_end; addr += PGSIZE) {
    if (!is_user_vaddr(addr)) exit(-1);

#ifdef VM
    /* 이 페이지에서 버퍼가 실제로 요구하는 마지막 바이트(스택 성장 판정에 사용) */
    uint8_t *range_end = addr + PGSIZE - 1;
    if (range_end > (uint8_t *)end_addr) range_end = (uint8_t *)end_addr;

    /* 1) SPT에서 존재 검사 - 시현님 get_safe_buffer 해결 로직 아이디어 */
    struct page *page = spt_find_page(spt, addr);

    /* 2) 없으면 스택 성장 기회 부여 (syscall에서도 user RSP 스냅샷 사용) */
    if (page == NULL) {
      if (!vm_try_stack_growth(range_end, curr->user_rsp)) {
        /* 성장 불가라면 잘못된 버퍼 */
        exit(-1);
      }
      page = spt_find_page(spt, addr);
      if (page == NULL) exit(-1);
    }

    /* 3) 쓰기 요청이면 writable 확인 */
    if (writable && !page->writable) exit(-1);

    /* 4) 아직 실제 매핑이 없다면 이제 매핑을 만든다 (lazy 상황 대비) */
    if (pml4_get_page(curr->pml4, addr) == NULL) {
      if (!vm_claim_page(addr)) exit(-1);
    }
#else
    /* Project 2(비-VM) 환경: 실제 PML4 매핑만 확인 */
    if (pml4_get_page(thread_current()->pml4, addr) == NULL) {
      exit(-1);
    }
#endif
  }
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

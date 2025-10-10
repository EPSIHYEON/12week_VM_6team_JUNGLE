#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "include/lib/string.h"
#include "intrinsic.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/gdt.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"

#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup(void);
static bool load(const char *file_name, struct intr_frame *if_);
static void initd(void *f_name);
static void __do_fork(void *);
// static struct lock filesys_lock;

/* General process initializer for initd and other process. */
static void process_init(void) { struct thread *current = thread_current(); }

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t process_create_initd(const char *file_name) {
  char *fn_copy;
  tid_t tid;

  char command_copy[64];
  char *program_command;
  char *str_point;

  strlcpy(command_copy, file_name, sizeof(command_copy));
  program_command = strtok_r(command_copy, " ", &str_point);

  /* Make a copy of FILE_NAME.
   * Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL) return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(program_command, PRI_DEFAULT, initd, fn_copy);
  if (tid == TID_ERROR) palloc_free_page(fn_copy);

  return tid;
}

/* A thread function that launches first user process. */
static void initd(void *f_name) {
#ifdef VM
  supplemental_page_table_init(&thread_current()->spt);
#endif

  process_init();

  if (process_exec(f_name) < 0) PANIC("Fail to launch initd\n");
  NOT_REACHED();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t process_fork(const char *name, struct intr_frame *if_) {
  /* Clone current thread to new thread.*/
  struct thread *parent_thread = thread_current();
  tid_t child_tid;

  // sema_init(&parent_thread->fork_sema, 0);

  parent_thread->parent_if = if_;

  child_tid = thread_create(name, PRI_DEFAULT, __do_fork,
                            thread_current());  // sema_up 뒤에 sema_down 실행 , sema_down뒤에
                                                // sema_up 진행 둘다 동기화를 막아주는 것은 동일함

  if (child_tid == TID_ERROR) {
    return TID_ERROR;
  }
  struct thread *child = find_tid_in_children(child_tid);
  sema_down(&parent_thread->fork_sema);

  if (!parent_thread->fork_success) {
    list_remove(&child->children_elem);
    sema_up(&child->exit_sema);
    return TID_ERROR;
  }

  return child_tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool duplicate_pte(uint64_t *pte, void *va, void *aux) {
  // PageTableEntry , Virtual Adress, 부모구조체포인터
  struct thread *current = thread_current();
  struct thread *parent = (struct thread *)aux;
  void *parent_page;
  void *newpage;
  bool writable;

  /* 1. TODO: If the parent_page is kernel page, then return immediately. */
  //커널 영역은 모든 프로세스가 같은 커널 페이지 테이블을 참조하기에 복사할
  //필요가 없음
  if (is_kernel_vaddr(va)) return true;

  /* 2. Resolve VA from the parent's page map level 4. */
  parent_page = pml4_get_page(parent->pml4, va);
  if (parent_page == NULL) return false;

  /* 3. TODO: Allocate new PAL_USER page for the child and set result to
   *    TODO: NEWPAGE. */
  //유저 영역용 페이지를 새로 1개 할당하고, 내부 데이터를 0으로 초기화
  newpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (newpage == NULL) {
    return false;
  }
  /* 4. TODO: Duplicate parent's page to the new page and
   *    TODO: check whether parent's page is writable or not (set WRITABLE
   *    TODO: according to the result). */
  memcpy(newpage, parent_page, PGSIZE);

  /* 5. Add new page to child's page table at address VA with WRITABLE
   *    permission. */
  writable = is_writable(pte);
  if (!pml4_set_page(current->pml4, va, newpage, writable)) {
    /* 6. TODO: if fail to insert page, do error handling. */
    palloc_free_page(newpage);
    return false;
  }
  return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void __do_fork(void *aux) {
  struct intr_frame if_;
  struct thread *parent = (struct thread *)aux;
  struct thread *current = thread_current();  // current는 이제 자식
  /* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
  // struct intr_frame *parent_if;
  bool succ = true;

  /* 1. Read the cpu context to local stack. */
  // 1. 부모가 넘겨준 intr_frame 주소를 통해 내용을 복사
  memcpy(&if_, parent->parent_if, sizeof(struct intr_frame));
  if_.R.rax = 0;

  // 자식을 부모 리스트에 넣기
  // current->parent = parent;
  // list_push_back(&parent->children_list, &current->children_elem);

  /* 2. Duplicate PT */
  // 1. 자식의 페이지 테이블(PML4)을 위한 공간을 먼저 할당
  current->pml4 = pml4_create();
  if (current->pml4 == NULL) {
    succ = false;
    goto fork_end;
  }
  // 2. CPU의 시야를 이제 막 만든 자식의 (텅 빈) 페이지 테이블로 전환
  process_activate(current);
#ifdef VM
  supplemental_page_table_init(&current->spt);
  if (!supplemental_page_table_copy(&current->spt, &parent->spt)) goto error;
#else
  // 3. 부모의 페이지 테이블 내용을 자식의 페이지 테이블로 복사
  if (!pml4_for_each(parent->pml4, duplicate_pte, parent)) {
    succ = false;
    goto fork_end;
  }
#endif

  /* TODO: Your code goes here.
   * TODO: Hint) To duplicate the file object, use `file_duplicate`
   * TODO:       in include/filesys/file.h. Note that parent should not return
   * TODO:       from the fork() until this function successfully duplicates
   * TODO:       the resources of parent.*/
  // 3. 부모의 fdt 내용을 자식의 fdt로 복사
  current->fdt[0] = parent->fdt[0];
  current->fdt[1] = parent->fdt[1];
  for (int i = 2; i < FDT_SIZE; i++) {
    struct file *file = parent->fdt[i];
    if (file != NULL) {
      current->fdt[i] = file_duplicate(file);

      if (current->fdt[i] == NULL) {
        succ = false;

        for (int j = 2; j < i; j++) {
          if (current->fdt[j] != NULL) file_close(current->fdt[j]);
        }
        goto fork_end;
      }
    }
  }

  goto fork_end;

  /* Finally, switch to the newly created process. */

error:
  thread_exit();
fork_end:
  parent->fork_success = succ;
  sema_up(&parent->fork_sema);

  if (!succ) {
    // current->is_wrong = 1;
    thread_exit();
  }

  process_init();
  do_iret(&if_);
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int process_exec(void *f_name) {
  char *file_name = f_name;
  bool success;

  /* We cannot use the intr_frame in the thread structure.
   * This is because when current thread rescheduled,
   * it stores the execution information to the member. */
  struct intr_frame _if;
  _if.ds = _if.es = _if.ss = SEL_UDSEG;
  _if.cs = SEL_UCSEG;
  _if.eflags = FLAG_IF | FLAG_MBS;

  if (is_kernel_vaddr(f_name)) {
    /* We first kill the current context */
    process_cleanup();
  }

  lock_acquire(&filesys_lock);
  /* And then load the binary */
  success = load(file_name, &_if);

  lock_release(&filesys_lock);

  /* If load failed, quit. */
  if (!success) {
    exit(-1);
  }

  /* Start switched process. */

  do_iret(&_if);  //유저 모드 진입
  NOT_REACHED();
}

/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int process_wait(tid_t child_tid) {
  struct thread *curr = thread_current();
  struct thread *t = find_tid_in_children(child_tid);

  if (t == NULL) {
    return -1;
  } else {
    // printf("t는 있어요.");
    sema_down(&t->wait_sema);        // 커널이 BLOCK
    int status = t->exit_status;     // RUNNING 중 정보 먹음
    list_remove(&t->children_elem);  // 이제 유저프로그램 정보 필요 없어
    sema_up(&t->exit_sema);          // 좀비 프로세스 깨워

    // while(1);
    // return -1;
    return status;
  }
}

/* Exit the process. This function is called by thread_exit (). */
void process_exit(void) {
  struct thread *curr = thread_current();

  // 실행중인 프로그램 파일 닫기 전에 쓰기 금지를 해제
  if (curr->running_file != NULL) {
    file_allow_write(curr->running_file);
    file_close(curr->running_file);
    curr->running_file = NULL;
  }

  for (int fd = 2; fd < FDT_SIZE; fd++) {
    if (curr->fdt[fd] != NULL) {
      file_close(curr->fdt[fd]);

      // curr->fdt[fd] = NULL;
    }
  }
  /* TODO: Your code goes here.
   * TODO: Implement process termination message (see
   * TODO: project2/process_termination.html).
   * TODO: We recommend you to implement process resource cleanup here. */
  // printf("process_exit ");

  sema_up(&curr->wait_sema);    // 세마 up 하고 좀비 프로세스가 됨
  sema_down(&curr->exit_sema);  //그 뒤에 BLOCK되서 커널이 깨워줘야 함
  process_cleanup();
}

/* Free the current process's resources. */
static void process_cleanup(void) {
  struct thread *curr = thread_current();

#ifdef VM
  supplemental_page_table_kill(&curr->spt);
#endif

  uint64_t *pml4;
  /* Destroy the current process's page directory and switch back
   * to the kernel-only page directory. */
  pml4 = curr->pml4;
  if (pml4 != NULL) {
    /* Correct ordering here is crucial.  We must set
     * cur->pagedir to NULL before switching page directories,
     * so that a timer interrupt can't switch back to the
     * process page directory.  We must activate the base page
     * directory before destroying the process's page
     * directory, or our active page directory will be one
     * that's been freed (and cleared). */
    curr->pml4 = NULL;
    pml4_activate(NULL);
    pml4_destroy(pml4);
  }
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void process_activate(struct thread *next) {
  /* Activate thread's page tables. */
  pml4_activate(next->pml4);

  /* Set thread's kernel stack for use in processing interrupts. */
  tss_update(next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
  unsigned char e_ident[EI_NIDENT];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct ELF64_PHDR {
  uint32_t p_type;
  uint32_t p_flags;
  uint64_t p_offset;
  uint64_t p_vaddr;
  uint64_t p_paddr;
  uint64_t p_filesz;
  uint64_t p_memsz;
  uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack(struct intr_frame *if_);
static bool validate_segment(const struct Phdr *, struct file *);
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool load(const char *file_name,
                 struct intr_frame *if_) {  //커널 모드에서 유저모드로 전환하기 위해
                                            //(유저)스택에 메모리를 할당하는 과정
  struct thread *t = thread_current();
  struct ELF ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  char *program_command;
  char *str_point;

  char *fn_copy = palloc_get_page(PAL_ZERO);
  if (fn_copy == NULL) return false;

  // 2. 유저의 데이터를 커널 페이지로 안전하게 복사
  strlcpy(fn_copy, file_name, PGSIZE);

  // 3. 이제 안전한 '복사본'을 가지고 마음껏 수정하며 사용
  program_command = strtok_r(fn_copy, " ", &str_point);

  if (program_command == NULL) {
    goto done;
  }
  /* Allocate and activate page directory. */  //페이지 테이블 매핑
  t->pml4 = pml4_create();
  if (t->pml4 == NULL) goto done;

  process_activate(thread_current());

  /* Open executable file. */  //파일을 염(비어있는지만 확인)
  file = filesys_open(program_command);
  if (file == NULL) {
    printf("load: %s: open failed\n", program_command);
    goto done;
  }

  file_deny_write(file);
  t->running_file = file;

  /* Read and verify executable header. */  //읽으면서 살행가능한지 확인
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\2\1\1", 7) || ehdr.e_type != 2 ||
      ehdr.e_machine != 0x3E  // amd64
      || ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */  //헤더를 읽고 어떤 주소로 보내야할지 결정하고
                               //보냄 (스택에 내용물 저장)
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file)) goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr) goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint64_t file_page = phdr.p_offset & ~PGMASK;
          uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint64_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
             * Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
             * Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void *)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */  //스택 할당(빈 공간)
  if (!setup_stack(if_)) goto done;

  /* Start address. */  //시작 위치 지정
  if_->rip = ehdr.e_entry;

  /* TODO: Your code goes here.
   * TODO: Implement argument passing (see project2/argument_passing.html). */

  // parsing
  char *args[64];
  args[0] = program_command;
  char *target;
  int num = 1;

  while ((target = strtok_r(NULL, " ", &str_point)) != NULL) {
    if (num >= 64) {  // args 배열이 꽉 찼으면 더 이상 받지 않음
      break;
    }
    args[num] = target;
    num++;
  }
  const int argc = num;
  char *args_address[64];

  // put into stack
  //실제 값을 스택에 채워넣는 과정
  num = num - 1;
  int temp_num = num;

  // 실제값 스택에 넣기
  while (temp_num >= 0) {
    if_->rsp -= strlen(args[temp_num]) + 1;  //길이 + \0 만큼 내리고 거기다가 공간 넣을 꺼임
    memcpy((void *)if_->rsp, args[temp_num], strlen(args[temp_num]) + 1);
    args_address[temp_num] = if_->rsp;
    temp_num--;
  }

  //실제값이 있는 주소값을 할당
  if_->rsp = if_->rsp & ~0x7;  // 8배수 정렬 (0으로 바뀌는 것 밖에 없으니
                               // 주소값이 같거나 작아지니까 안심!)

  temp_num = num;

  if_->rsp -= 8;
  *((void **)if_->rsp) = NULL;  // 끝을 알리는 값

  while (temp_num >= 0) {
    if_->rsp -= 8;
    *(void **)if_->rsp = args_address[temp_num];  //주소값 자체를 8바이트 공간에 그냥 써넣는 것
    // args_address[temp_num] 라는 주소로 가서, 거기 있는 8바이트를 읽어서
    // 스택에 복사 그 주소는 우리가 접근할 수 없는 곳일 확률이 높음(memcpy가
    // 안되는 이유)
    temp_num--;
  }

  if_->R.rdi = argc;                //총 갯수를 rdi에게 (0도 포함해야하기 때문에)
  if_->R.rsi = (uint64_t)if_->rsp;  // 시작 주소를 rsi에게

  if_->rsp -= 8;
  *(void **)if_->rsp = 0;  // main은 돌아갈 지점이 없기 때문에 거짓 주소를 반환해줌

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  palloc_free_page(fn_copy);
  // 이미 file_deny_write()가 걸려 있었다면 이를 되돌린 뒤 파일을 닫도록 수정
  if (!success && file != NULL) {
    file_allow_write(file);
    file_close(file);
    t->running_file = NULL;
  }
  return success;
}

/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Phdr *phdr, struct file *file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (uint64_t)file_length(file)) return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0) return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void *)phdr->p_vaddr)) return false;
  if (!is_user_vaddr((void *)(phdr->p_vaddr + phdr->p_memsz))) return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr) return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE) return false;

  /* It's okay. */
  return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page(void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
//전체 건물 시공팀 (필요한 all pages를 만듬)
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Do calculate how to fill this page.
     * We will read PAGE_READ_BYTES bytes from FILE
     * and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t *kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL) return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      printf("fail\n");
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool setup_stack(struct intr_frame *if_) {
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
    if (success)
      if_->rsp = USER_STACK;
    else
      palloc_free_page(kpage);
  }
  return success;
}

int process_add_file(struct file *file_open) {}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool install_page(void *upage, void *kpage, bool writable) {
  struct thread *t = thread_current();

  /* Verify that there's not already a page at that virtual
   * address, then map our page there. */
  return (pml4_get_page(t->pml4, upage) == NULL && pml4_set_page(t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

// load_segment가 남긴 약속을 이행
//층별 전문 시공팀(a single page를 만듬, page 마다의 aux를 받아서 시공)
static bool lazy_load_segment(struct page *page, void *aux) {
  /* TODO: Load the segment from the file */
  /* TODO: This called when the first page fault occurs on address VA. */
  /* TODO: VA is available when calling this function. */

  struct aux *aux_data = (struct aux *)aux;
  struct file *file = aux_data->file;
  off_t ofs = aux_data->ofs;
  size_t page_read_bytes = aux_data->read_bytes;
  size_t page_zero_bytes = aux_data->zero_bytes;
  uint8_t *kpage = page->frame->kva;

  if (file_read_at(file, kpage, page_read_bytes, ofs) != (int)page_read_bytes) {
    return false;
  }
  //나머지 공간은 0으로 채움
  memset(kpage + page_read_bytes, 0, page_zero_bytes);

  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
//프로그램을 처음 디스크에서 읽어올 때, 실행 파일(ELF)에 명시된 초기 레이아웃을
//설정하는 역할
static bool load_segment(struct file *file, off_t ofs, uint8_t *upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) {
    /* Do calculate how to fill this page.
     * We will read PAGE_READ_BYTES bytes from FILE
     * and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* TODO: Set up aux to pass information to the lazy_load_segment. */
    // void *aux = NULL;
    struct aux *aux = malloc(sizeof(struct aux));
    if (aux == NULL) {
      return false;
    }
    aux->file = file_reopen(file);
    aux->ofs = ofs;
    aux->read_bytes = page_read_bytes;
    aux->zero_bytes = page_zero_bytes;

    if (!vm_alloc_page_with_initializer(VM_FILE,
                                        upage,  //원래는 VM_ANON 주의!!!!!!!!!
                                        writable, lazy_load_segment, aux)) {
      free(aux);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
    ofs += PGSIZE;
  }
  return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
static bool setup_stack(struct intr_frame *if_) {
  bool success = false;
  /* 페이지 하나 크기만큼 프레임 할당 받음 -> 그 페이지의 시작주소
   * RSP 는 수정 X 스택의 가장 높은 주소를 가르키고 있어야함  */
  void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

  /* TODO: Map the stack on stack_bottom and claim the page immediately.
   * TODO: If success, set the rsp accordingly.
   * TODO: You should mark the page is stack. */
  /* TODO: Your code goes here */
  /* SPT에 페이지 할당: VM_MARKER_0는 스택이란걸 표시하는 용도  */
  if (!vm_alloc_page(VM_ANON, stack_bottom, true)) {
    return false;
  }
  success = vm_claim_page(stack_bottom);
  if (success) {
    if_->rsp = USER_STACK;
  }
  return success;
}
#endif /* VM */

int get_fd_from_fdt(struct file *file_open) {
  struct thread *t = thread_current();
  for (int fd = 2; fd < FDT_SIZE; fd++) {
    if (t->fdt[fd] == NULL) {
      t->fdt[fd] = file_open;
      return fd;
    }
  }

  return -1;
}

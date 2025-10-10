#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H


#include <stdbool.h>

void syscall_init (void);
int open (const char *file);
void close (int fd);
int filesize(int fd);
int read (int fd, void *buffer, unsigned size);
unsigned tell (int fd);
void seek (int fd, unsigned position) ;
int exec (void *f_name);
void exit (int status);
bool sys_remove (const char *file);
extern struct lock filesys_lock;


#endif /* userprog/syscall.h */

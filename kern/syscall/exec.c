#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <thread.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <synch.h>
#include <kern/unistd.h>
#include <copyinout.h>
#include <spl.h>

//TODO: Stack pointer == argv pointer
//TODO: memory limits; see error msgs; see limit.h
//TODO: set up stdio?

int sys_execv(userptr_t progname, userptr_t args){
    struct vnode *v;
    vaddr_t entrypoint, stackptr;
    userptr_t userdest;
    int result, i, len, argc, pad, spl;
    size_t got;

    struct addrspace *old_addr = curthread->t_addrspace;
    char **usr_args = (char**)args;

    // Count argc
    argc = 0;
    while(usr_args[argc] != NULL){
        argc++;
    }

    char *args_buf[argc+1];
    userptr_t user_argv[argc+1];

    // Turn interrupts off to prevent multiple execs from executing to save space
    spl = splhigh();

    /* Open the file. */
    result = vfs_open((char *)progname, O_RDONLY, 0, &v);
    if (result) {
        goto err1;
    }

    // Keep old addrspace in case of failure
    struct addrspace *new_addr = as_create();
    if (new_addr == NULL){
        result = ENOMEM;
        goto err2;
    }

    // Copy args to kernel with copyinstr; The array is terminated by a NULL
    // The args argument is an array of 0-terminated strings.
    i = 0;
    while (usr_args[i] != NULL){
        args_buf[i] = kmalloc(ARG_MAX*sizeof(char));
        if (args_buf[i] == NULL){
            result = ENOMEM;
            goto err3;
        }
        result = copyinstr((const_userptr_t)usr_args[i], args_buf[i], ARG_MAX, &got);
        if (result){
            goto err3;
        }
        i++;
    }

    // Swap addrspace
    curthread->t_addrspace = new_addr;
    as_activate(curthread->t_addrspace);

    /* Load the executable. */
    result = load_elf(v, &entrypoint);
    if (result) {
        goto err4;
    }

    /* Define the user stack in the address space */
    result = as_define_stack(curthread->t_addrspace, &stackptr);
    if (result) {
        goto err4;
    }

    // Copy args to new addrspace
    for (i=argc-1; i>-1; i--){
        len = strlen(args_buf[i]) + 1;
        pad = (4 - (len%4) ) % 4; // Word align

        if (i==argc-1){
            user_argv[i] = (userptr_t)(stackptr - len - pad);
        }
        else{
            user_argv[i] = (userptr_t)(usr_args[i+1] - len - pad);
        }

        result = copyoutstr((const char*)args_buf[i], user_argv[i], len, &got);
        if (result){
            goto err4;
        }
    }

    // Copy pointers to argv
    userdest = user_argv[0] - 4 * (argc+1);
    stackptr = (vaddr_t)userdest; // Set stack pointer
    for (i=0; i<argc+1; i++){
        result = copyout((const void *)&user_argv[i], userdest, 4);
        if (result)
            goto err4;
        userdest += 4;
    }

    // Wrap up
    for (i=0; i<argc+1; i++)
        kfree(args_buf[i]);
    vfs_close(v);
    splx(spl);

    /* Warp to user mode. */
    enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
              stackptr, entrypoint);

    /* enter_new_process does not return. */
    return EINVAL;

    err4:
        curthread->t_addrspace = old_addr;
        as_activate(curthread->t_addrspace);
    err3:
        for (i=0; i<argc+1; i++){
            if (args_buf[i] != NULL)
                kfree(args_buf[i]);
        }
        as_destroy(new_addr);
    err2:
        vfs_close(v);
    err1:
        splx(spl);
        return result;
}
//
// Copyright (C) 2001/2005 The Honeynet Project.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//



#include "syscall.h"


static u32 **    orig_sys_call_table;
#if ( LINUX_VERSION_CODE > KERNEL_VERSION(2,6,10) )
struct {
        unsigned short limit;
        unsigned int base;
} __attribute__ ((packed)) idtr;

struct {
        unsigned short off1;
        unsigned short sel;
        unsigned char none,flags;
        unsigned short off2;
} __attribute__ ((packed)) idt;
#define READ_ASM 128     /* How far to read into asm */

#endif

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) )

#define ASMIDType( val ) \
    __asm__ ( val );

#define JmPushRet( val )     \
    ASMIDType          \
    (              \
        "push %0   \n"     \
        "ret       \n"     \
                   \
        : : "m" (val)    \
    );

#define CallHookedSyscall( val ) \
    ASMIDType( "call *%0" : : "r" (val) );

void *sysenter_entry;
void *system_call;
unsigned long before_exit;
unsigned long before_call;
unsigned long after_call;

void set_before_and_after_calls(void *system_call);
void set_idt_handler(void *system_call);
void set_sysenter_handler(void *sysenter);
unsigned int get_sysenter_entry(void *syscall_call, void *syscall_table);
void new_idt(void);
void hook(void);
#endif


//----- ptr to the original reads
asmlinkage static ssize_t (*ord)  (unsigned int,char *,size_t);
asmlinkage static ssize_t (*ordv) (unsigned int,const struct iovec * ,size_t);
asmlinkage static ssize_t (*oprd64) (unsigned int, char *,size_t, off_t);
//----- ptr to the original writes
asmlinkage static ssize_t (*owr)  (unsigned int,const char *,size_t);
asmlinkage static ssize_t (*owrv) (unsigned int,const struct iovec * ,size_t);
asmlinkage static ssize_t (*opwr64) (unsigned int,const char *,size_t, off_t);

//---- sock_call multiplexor function pointers
asmlinkage static long (*osk) (int call, unsigned long *args);
//----- orig open call
asmlinkage static int (*oo)(const char * filename, int flags, int mode);
//----- origfork
asmlinkage static int (*ofk)(struct pt_regs regs);
asmlinkage static int (*ovfk)(struct pt_regs regs);
asmlinkage static int (*oclone)(struct pt_regs regs);


#if ( LINUX_VERSION_CODE > KERNEL_VERSION(2,6,10) )
// implementation of memmem since the kernel doesn't provide one for me
// used to find the call <something>(,eax,4) after the idt (something) is
// the location of the sys_call_table
void *memmem(const void* haystack, size_t hl, const void* needle, size_t nl)
{
    int i;
    if (nl>hl) return 0;
    for (i=hl-nl+1; i; --i) {
        if (!memcmp(haystack,needle,nl))
            return (char*)haystack;
        ++haystack;
    }
    return 0;
}

u32 find_system_call(void)
{
   u32 sys_call_off = 0;

   /* ask the processor for the idt address and store it in idtr */
   asm ("sidt %0" : "=m" (idtr));

   /* read in IDT for int 0x80 (syscall) */
   memcpy(&idt,(void *)idtr.base+8*0x80,sizeof(idt));
   sys_call_off = (idt.off2 << 16) | idt.off1;

   return sys_call_off;
}

u32 **find_sys_call_table(u32 sc_asm)
{
    char *p;

    /* we have syscall routine address now, look for syscall table
       dispatch (indirect call) */
    p = (char*)memmem ((void *)sc_asm,READ_ASM,"\xff\x14\x85",3);

    if (p)
    {
        return (u32 **)*(unsigned*)(p+3);
    }
    else
    {
        return NULL;
    }
}
#endif

u32** get_sct(void){

#if ( LINUX_VERSION_CODE > KERNEL_VERSION(2,6,10) )
   u32 l_system_call = find_system_call();
   u32 **l_sc_table = find_sys_call_table(l_system_call);
   return l_sc_table;
#else
  unsigned long ptr;
  //extern int loops_per_jiffy;
  extern unsigned long loops_per_jiffy;

  for (ptr = (unsigned long)&loops_per_jiffy;
       ptr < (unsigned long)&boot_cpu_data; ptr += sizeof(void *)){

    unsigned long *p;
    p = (unsigned long *)ptr;
    //---- orig ver that looked for sys_exit didnt work on stock
    //---- kerns.
    if (p[__NR_close] == (u32) sys_close){
       return  (u32 **)p;
    }

  }
#endif

  return 0;
}



//----- nfk:   New fork, this calls the old fork and records the parent
//-----           to child relations when no associated read happens.
asmlinkage int nfk(struct pt_regs regs){
  
  int retval;
  atomic_inc(&refcount);

  //--- call the old fork
  retval = ofk(regs);

  //--- at this point this puppy should return twice, we only need
  //--- to record the child
  if(retval == 0){
    //----- this is the child process, lets log a dummy read record.
    sbk_log(SBK_READ,0,0,0,NULL,0);
    sbk_filter_fork();
  }


  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return retval;
}



//----- nclone:   New vform, this calls the old vfork and records the parent
//-----           to child relations when no associated read happens.
asmlinkage int nvfk(struct pt_regs regs){

  int retval;

  atomic_inc(&refcount);

  //--- call the old fork
  retval = ovfk(regs);

  //--- at this point this puppy should return twice, we only need
  //--- to record the child
  if(retval == 0){
    //----- this is the child process, lets log a dummy read record.
    sbk_log(SBK_READ,0,0,0,NULL,0);
    sbk_filter_fork();
  } 

  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return retval;
}


//----- nclone:   New clone, this calls the old clone and records the parent
//-----           to child relations when no associated read happens.
asmlinkage int nclone(struct pt_regs regs){
  
  int retval; 

  atomic_inc(&refcount);
  
  //--- call the old fork
  retval = oclone(regs);


  //--- at this point this puppy should return twice, we only need
  //--- to record the child
  if(retval == 0){
    //----- this is the child process, lets log a dummy read record.
    sbk_log(SBK_READ,0,0,0,NULL,0);
    sbk_filter_fork();
  } 
  
  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return retval;
}




//----- no:   New Open, this calls the old open call and records the filename
//-----       to fd and inode mapping.
asmlinkage int  no(const char * filename, int flags, int mode){
  
  long retval;
  unsigned long inode;


  int pathmax;
  int len;
  char * buffer;
  char * path;
  int action;

  atomic_inc(&refcount);

  retval = oo(filename,flags,mode);  
 
  if(retval >= 0){
    //------ open call worked
    //--mark for filtering always!!
    sbk_filter_open(fcheck_files(current->files,retval));   
    
    //printk(KERN_ALERT "Sebek - about to eval filt\n");
    action=sbk_filter_eval(retval);
    //printk(KERN_ALERT "Sebek - filter eval done\n");
    //----- no action needed
    if(action == SBK_FILT_ACT_IGNORE) goto OUT;
    //----- no action needed if we are KSO and it doesnt look like keystrokes
    //if(action == SBK_FILT_ACT_KSO  && r > 1)goto OUT;


    //----- figure out our pathname max.    
    pathmax = BUFLEN - sizeof(struct sbk_h);
    buffer    = kmalloc(pathmax,GFP_KERNEL);

    if(!buffer)goto OUT;

    //------ get inode;
    inode = fd2inode(retval);



    //----- get full pathname that corresponds to the inode
    path = fd2path(retval,buffer,pathmax);

    //----- get the the real length of the path, if its too big, truncate.
    len = strlen(path);
    if(len > pathmax)len = pathmax;

    sbk_log(SBK_OPEN,retval,inode,len,(const u_char *)path,0);

    kfree(buffer);
  }


 OUT:

  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return retval;

}



//----- nrd:  New Read, this calls the old read call then records all the
//-----       interesting data.  It uses the log function for recording.
asmlinkage ssize_t nrd (unsigned int fd, char *buf, size_t count) {

  ssize_t r;
  char * ptr;
  int action;

  u_int32_t bufsize;
  u_int32_t inode;

  atomic_inc(&refcount);

  //----- run original sys_read....
  r = ord(fd, buf, count);


  //----- check for error and interest
  if(r < 1 || ((BLOCK[KSO_OFFSET] & 0x00000001) && r > 1))goto OUT;

  
  //--Filter Code Follows
  //--Determine action
  //printk(KERN_ALERT "Sebek - about to eval filt\n");
  action=sbk_filter_eval(fd);
  //printk(KERN_ALERT "Sebek - filter eval done\n");
  //----- no action needed
  if(action == SBK_FILT_ACT_IGNORE) goto OUT;
  //----- no action needed if we are KSO and it doesnt look like keystrokes
  if(action == SBK_FILT_ACT_KSO  && r > 1)goto OUT;



  //----- log the read contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

  
  //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);

  if(r < bufsize){

    //----- data is less than buffer size, we can copy it in single step
    sbk_log(SBK_READ,fd,inode,r,buf,1);
    
  }else{

    //----- data is > buffer size, need to nibble at it
    for(ptr = buf; ptr + bufsize  <= buf + r ; ptr+= bufsize){
      sbk_log(SBK_READ,fd,inode,bufsize,ptr,1);
    }

    //----- dont forget the remainder
    sbk_log(SBK_READ,fd,inode,r % bufsize,ptr,1);
  }
  

 OUT:

  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return r;  
}


//----- nrdv:  New Readv, this calls the old readv call then records all the
//-----       interesting data.  It uses the log function for recording.
asmlinkage ssize_t nrdv (unsigned int fd, const struct iovec * vector , size_t count) {

  ssize_t r;
  ssize_t len;
  size_t  i;
  void * ptr;
  u_int32_t bufsize;
  u_int32_t inode;
  struct iovec * iov;
  int action;

  atomic_inc(&refcount);
 
  //----- run original sys_read....
  r = ordv(fd, vector, count);
 
  //----- check for error and interest
  if(r < 1 || ((BLOCK[KSO_OFFSET] & 0x00000001) && r > 1) ||  (count > UIO_MAXIOV))goto OUT;

  //--Filter Code Follows
  //--Determine action
  action=sbk_filter_eval(fd);
  //----- no action needed
  if(action == SBK_FILT_ACT_IGNORE) goto OUT;
  //----- no action needed if we are KSO and it doesnt look like keystrokes
  if(action == SBK_FILT_ACT_KSO  && r > 1)goto OUT;


 
  //----- allocate iovec buffer
  iov = kmalloc(count*sizeof(struct iovec), GFP_KERNEL);
  if (!iov)goto OUT;


  //----- copy over iovec struct
  if (copy_from_user(iov, vector, count*sizeof(*vector)))goto OUT_W_FREE;


  //----- log the read contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

  //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);


  for(i = 0; i < count; i++){
    len = iov[i].iov_len;
    
    if(len < bufsize){
      
      //----- data is less than buffer size, we can copy it in single step
      sbk_log(SBK_READ,fd,inode,r,iov[i].iov_base,1);
      
    }else{
      
      //----- data is > buffer size, need to nibble at it
      for(ptr = iov[i].iov_base; ptr + bufsize  <= iov[i].iov_base + r ; ptr+= bufsize){
	sbk_log(SBK_READ,fd,inode,bufsize,ptr,1);
      }
      
      //----- dont forget the remainder
      sbk_log(SBK_READ,fd,inode,r % bufsize,ptr,1);
    }
  }

 OUT_W_FREE:
  kfree(iov);

 OUT:

  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);
  
  return r;  
}



//----- nprd:  New Read, this calls the old pread call then records all the
//-----       interesting data.  It uses the log function for recording.
asmlinkage ssize_t nprd64 (unsigned int fd, char *buf, size_t count, off_t offset) {

  ssize_t r;
  char * ptr;
  u_int32_t bufsize;
  u_int32_t inode;
  int action;

  atomic_inc(&refcount);
  
  //----- run original sys_read....
  r = oprd64(fd, buf, count, offset);
 

  //----- check for error and interest
  if(r < 1 || ((BLOCK[KSO_OFFSET] & 0x00000001) && r > 1))goto OUT;

 //--Filter Code Follows
  //--Determine action
  action=sbk_filter_eval(fd);
  //----- no action needed
  if(action == SBK_FILT_ACT_IGNORE) goto OUT;
  //----- no action needed if we are KSO and it doesnt look like keystrokes
  if(action == SBK_FILT_ACT_KSO  && r > 1)goto OUT;


 
  //----- log the read contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

   //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);


  if(r < bufsize){

    //----- data is less than buffer size, we can copy it in single step
    sbk_log(SBK_READ,fd,inode,r,buf,1);
    
  }else{

    //----- data is > buffer size, need to nibble at it
    for(ptr = buf; ptr + bufsize  <= buf + r ; ptr+= bufsize){
      sbk_log(SBK_READ,fd,inode,bufsize,ptr,1);
    }

    //----- dont forget the remainder
    sbk_log(SBK_READ,fd,inode,r % bufsize,ptr,1);
  }
  

 OUT:
  
  if(atomic_dec_and_test(&refcount))
     wake_up_interruptible(&wait);

  return r;  
}


//--------------------------- WRITE SYSCALLS (BEGIN) -----------------------
//
// Author: Raul Siles (raul@raulsiles.com)
// Acks:   This is the result of a Honeynet research project between:
//         Telefonica Moviles España (TME) & Hewlett-Packard España (HPE)
// -------

//----- nwr:  New Write, this calls the old write call then records all the
//-----       interesting data.  It uses the log function for recording.
 
ssize_t nwr (unsigned int fd, const char *buf, size_t count) {

  ssize_t w;


  const char * ptr;

  u_int32_t bufsize;
  u_int32_t inode;

 
  //----- run original sys_write....
  w = owr(fd, buf, count);

  //----- check for error
  if(w < 1) return w;


  //----- log the write contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

  
  //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);

  if(w < bufsize){

    //----- data is less than buffer size, we can copy it in single step
    sbk_log(SBK_WRITE,fd,inode,w,buf,1);
    
  }else{

    //----- data is > buffer size, need to nibble at it
    for(ptr = buf; ptr + bufsize  <= buf + w ; ptr+= bufsize){
      sbk_log(SBK_WRITE,fd,inode,bufsize,ptr,1);
    }

    //----- dont forget the remainder
    sbk_log(SBK_WRITE,fd,inode,w % bufsize,ptr,1);
  }
  
  return w;  
}


//----- nwrv: New Writev, this calls the old writev call then records all the
//-----       interesting data.  It uses the log function for recording.
 
inline ssize_t nwrv (unsigned int fd, const struct iovec * vector , size_t count) {

  ssize_t w;
  ssize_t len;
  size_t  i;

  void * ptr;

  u_int32_t bufsize;
  u_int32_t inode;

  struct iovec * iov;

  
 
  //----- run original sys_write....
  w = owrv(fd, vector, count);

 
  //----- check for error
  if(w < 1 || (count > UIO_MAXIOV))goto OUT;

 
  //----- allocate iovec buffer
  iov = kmalloc(count*sizeof(struct iovec), GFP_KERNEL);
  if (!iov)goto OUT;


  //----- copy over iovec struct
  if (copy_from_user(iov, vector, count*sizeof(*vector)))goto OUT_W_FREE;


  //----- log the write contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

  //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);

  for(i = 0; i < count; i++){
    len = iov[i].iov_len;
    
    if(len < bufsize){
      
      //----- data is less than buffer size, we can copy it in single step
      sbk_log(SBK_WRITE,fd,inode,w,iov[i].iov_base,1);
      
    }else{
      
      //----- data is > buffer size, need to nibble at it
      for(ptr = iov[i].iov_base; ptr + bufsize  <= iov[i].iov_base + w ; ptr+= bufsize){
	sbk_log(SBK_WRITE,fd,inode,bufsize,ptr,1);
      }
      
      //----- dont forget the remainder
      sbk_log(SBK_WRITE,fd,inode,w % bufsize,ptr,1);
    }
  }

 OUT_W_FREE:
  kfree(iov);

 OUT:
  return w;  
}



//----- npwr: New PWrite, this calls the old pwrite call then records all the
//-----       interesting data.  It uses the log function for recording.
 
inline ssize_t npwr64 (unsigned int fd, const char *buf, size_t count, off_t offset) {

  ssize_t w;

  const char * ptr;

  u_int32_t bufsize;
  u_int32_t inode;

 
  //----- run original sys_write....
  w = opwr64(fd, buf, count, offset);


  //----- check for error
  if(w < 1) return w;

 
  //----- log the write contents.  Including context data.    
  bufsize = BUFLEN - sizeof(struct sbk_h);

   //----- get inode , eventually this will drive filtering.
  inode = fd2inode(fd);


  if(w < bufsize){

    //----- data is less than buffer size, we can copy it in single step
    sbk_log(SBK_WRITE,fd,inode,w,buf,1);
    
  }else{

    //----- data is > buffer size, need to nibble at it
    for(ptr = buf; ptr + bufsize  <= buf + w ; ptr+= bufsize){
      sbk_log(SBK_WRITE,fd,inode,bufsize,ptr,1);
    }

    //----- dont forget the remainder
    sbk_log(SBK_WRITE,fd,inode,w % bufsize,ptr,1);
  }
  
  return w;  
}
//--------------------------- WRITE SYSCALLS (END) -------------------------







//----- nsk:  New Socket, this calls the old socket call and then logs
//-----      who is connected to the other end of the socket.
asmlinkage long nsk(int call,unsigned long __user *args){

        #define AL(x) ((x) * sizeof(unsigned long))
	static unsigned char nargs[18]={AL(0),AL(3),AL(3),AL(3),AL(2),AL(3),
		                        AL(3),AL(3),AL(4),AL(4),AL(4),AL(6),
			                AL(6),AL(2),AL(5),AL(5),AL(3),AL(3)};
        #undef AL
										
	long retval;
	unsigned long a[6];
  	struct msghdr msg;
	struct sockaddr_in  inaddr;

	atomic_inc(&refcount);


	//--- old socket call
	retval = osk(call,args);

	if(call<1||call>SYS_RECVMSG){
                retval = -EINVAL;
		goto OUT;
	}
	
	if(!copy_from_user(a,args,nargs[call])){
	
	  switch(call){
		case SYS_CONNECT:
		case SYS_LISTEN:
			sock_track(call,a[0],0,0);	
			break;
		case SYS_ACCEPT:
			//---- the fd associated with the accept call 
			//---- is not interesting its the return val
			//---- which refereces the new connection
			sock_track(call,retval,0,0);
			break;
		case SYS_SENDMSG:
		case SYS_RECVMSG:
			if (copy_from_user(&msg,(void *)a[1],sizeof(struct msghdr)))
		            goto OUT;

			if (msg.msg_namelen > __SOCK_SIZE__ ||
			    copy_from_user(&inaddr,(struct sockaddr *)msg.msg_name,msg.msg_namelen))
			    goto OUT;
	
			if(inaddr.sin_family == AF_INET){		
			  sock_track(call,a[0],inaddr.sin_addr.s_addr,inaddr.sin_port);
			}
			break;
		case SYS_SENDTO:
		case SYS_RECVFROM:
		      if (copy_from_user(&msg,(void *)a[1],sizeof(struct msghdr)))
		         goto OUT;

		      if (a[5] > __SOCK_SIZE__ || 
		         copy_from_user(&inaddr,(struct sockaddr *)a[4],a[5]))
		         goto OUT;

                      if(inaddr.sin_family == AF_INET){
                        sock_track(call,a[0],inaddr.sin_addr.s_addr,inaddr.sin_port);
                      }
                      break;
	  }
	}

 OUT:
	
	
	if(atomic_dec_and_test(&refcount))
	  wake_up_interruptible(&wait);
	
	return retval;
}



//----- init_monitoring:  initializer for system call monitoring
//-----                   currently just clones the syscall table 
//-----                   
int init_monitoring(){

  //--- for now lets kick it old school
  orig_sys_call_table = get_sct();

  if(!orig_sys_call_table)return -1;

  ord                 = (void *)orig_sys_call_table[__NR_read];
  ordv                = (void *)orig_sys_call_table[__NR_readv];
  oprd64              = (void *)orig_sys_call_table[__NR_pread64];
  oo                  = (void *)orig_sys_call_table[__NR_open];
  osk                 = (void *)orig_sys_call_table[__NR_socketcall];
  ofk                 = (void *)orig_sys_call_table[__NR_fork];
  ovfk                = (void *)orig_sys_call_table[__NR_vfork];
  oclone              = (void *)orig_sys_call_table[__NR_clone];

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) )
  system_call = (void *)find_system_call();
  set_before_and_after_calls(system_call);

  sysenter_entry = (void *)get_sysenter_entry(system_call, orig_sys_call_table);
  if (sysenter_entry == NULL || sysenter_entry == 0)
  {
    //printk("Could not locate sysenter... Exiting!\n");
    return EINTR;
  }
#endif

  return 1;
}


int start_monitoring(){


#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) )
  set_idt_handler(system_call);
  set_sysenter_handler(sysenter_entry);
#else
  lock_kernel();

  orig_sys_call_table[__NR_read]       = (u32 *)nrd;
  orig_sys_call_table[__NR_readv]      = (u32 *)nrdv;
  orig_sys_call_table[__NR_pread64]    = (u32 *)nprd64;

  orig_sys_call_table[__NR_open]       = (u32 *)no;
 
  orig_sys_call_table[__NR_fork]       = (u32 *)nfk;
  orig_sys_call_table[__NR_vfork]      = (u32 *)nvfk;
  orig_sys_call_table[__NR_clone]      = (u32 *)nclone;

  if(BLOCK[WRITE_OFFSET] & 0x00000001){
    orig_sys_call_table[__NR_write]       = (u32 *)nwr;
    orig_sys_call_table[__NR_writev]      = (u32 *)nwrv;
    orig_sys_call_table[__NR_pwrite64]    = (u32 *)npwr64;
  }

  if(BLOCK[SOCKET_OFFSET] & 0x00000001){
    orig_sys_call_table[__NR_socketcall] = (u32 *)nsk;
  }

  unlock_kernel();
#endif

  return 1;
}


int stop_monitoring(){

  lock_kernel();

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) )
  //should return idt to its normal state
#else
  orig_sys_call_table[__NR_read]       = (u32 *)ord;
  orig_sys_call_table[__NR_readv]      = (u32 *)ordv;
  orig_sys_call_table[__NR_pread64]    = (u32 *)oprd64;

  orig_sys_call_table[__NR_open]       = (u32 *)oo;
 
  orig_sys_call_table[__NR_fork]       = (u32 *)ofk;
  orig_sys_call_table[__NR_vfork]      = (u32 *)ovfk;
  orig_sys_call_table[__NR_clone]      = (u32 *)oclone;

  if(BLOCK[WRITE_OFFSET] & 0x00000001){
    orig_sys_call_table[__NR_write]       = (u32 *)owr;
    orig_sys_call_table[__NR_writev]      = (u32 *)owrv;
    orig_sys_call_table[__NR_pwrite64]    = (u32 *)opwr64;
  }
  

  if(BLOCK[SOCKET_OFFSET] & 0x00000001){
    orig_sys_call_table[__NR_socketcall] = (u32 *)osk;
  }
#endif

  unlock_kernel();

  return 1;
}

#if ( LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18) )
void set_before_and_after_calls(void *system_call)
{
  unsigned char *p;

  p = (unsigned char *) system_call;

  while (!((*p == 0xff) && (*(p+1) == 0x14) && (*(p+2) == 0x85)))
    p++;

  before_call = (unsigned long) p;

  /* pass syscall table */
  p += 7;

  after_call = (unsigned long) p;

  /* cli */
  while (*p != 0xfa)
    p++;

  before_exit = (unsigned long) p;
}

void set_idt_handler(void *system_call)
{
  unsigned char *p;
  unsigned long *p2;

  p = (unsigned char *) system_call;

  /* first jump */
  while (!((*p == 0x0f) && (*(p+1) == 0x83)))
    p++;

  p -= 5;

  *p++ = 0x68;
  p2 = (unsigned long *) p;
  *p2++ = (unsigned long) ((void *) new_idt);

  p = (unsigned char *) p2;
  *p = 0xc3;

  /* syscall_trace_entry jump */
  while (!((*p == 0x0f) && (*(p+1) == 0x82)))
    p++;

  p -= 5;

  *p++ = 0x68;
  p2 = (unsigned long *) p;
  *p2++ = (unsigned long) ((void *) new_idt);

  p = (unsigned char *) p2;
  *p = 0xc3;
}

void set_sysenter_handler(void *sysenter)
{
  unsigned char *p;
  unsigned long *p2;

  p = (unsigned char *) sysenter;

  /* looking for call */
  while (!((*p == 0xff) && (*(p+1) == 0x14) && (*(p+2) == 0x85)))
    p++;

  /* looking for jae syscall_badsys */
  while (!((*p == 0x0f) && (*(p+1) == 0x83)))
    p--;

  p -= 5;

  /* insert jump */
  *p++ = 0x68;
  p2 = (unsigned long *) p;
  *p2++ = (unsigned long) ((void *) new_idt);

  p = (unsigned char *) p2;
  *p = 0xc3;
}

unsigned int get_sysenter_entry(void *syscall_call, void *syscall_table)
{
  unsigned char *p = (unsigned char*)syscall_call;
  unsigned int verify = 0;

  while(!((p[0] == 0xff) && (p[1] == 0x14) && (p[2] == 0x85)))
  {
    p--;
  }

  verify = *(unsigned int *)(p+3);
  if (verify == (unsigned int)syscall_table)
  {
    return (unsigned int) p;
  }

  return 0;
}

void new_idt(void)
{
  ASMIDType
  (
    "cmp %0, %%eax      \n"
    "jae mysyscall        \n"
    "jmp hook               \n"

    "mysyscall:           \n"
    "jmp before_exit          \n"

    : : "i" (NR_syscalls)
  );
}

void hook(void)
{
  register int eax asm("eax");

  switch(eax)
  {
    case __NR_read:
      CallHookedSyscall(nrd);
      break;

    case __NR_readv:
      CallHookedSyscall(nrdv);
      break;
 
    case __NR_pread64:
      CallHookedSyscall(nprd64);
      break;

    case __NR_open:
      CallHookedSyscall(no);
      break;

    case __NR_fork:
      CallHookedSyscall(nfk);
      break;

    case __NR_vfork:
      CallHookedSyscall(nvfk);
      break;

    case __NR_clone:
      CallHookedSyscall(nclone);
      break;

    case __NR_write:
      if (BLOCK[WRITE_OFFSET] & 0x00000001)
      {
        CallHookedSyscall(nwr);
      }
      else
      {
        JmPushRet(before_call);
      }
      break;

    case __NR_writev:
      if (BLOCK[WRITE_OFFSET] & 0x00000001)
      {
        CallHookedSyscall(nwrv);
      }
      else
      {
        JmPushRet(before_call);
      }
      break;

    case __NR_pwrite64:
      if (BLOCK[WRITE_OFFSET] & 0x00000001)
      {
        CallHookedSyscall(npwr64);
      }
      else
      {
        JmPushRet(before_call);
      }
      break;

    case __NR_socketcall:
      if (BLOCK[SOCKET_OFFSET] & 0x00000001)
      {
        CallHookedSyscall(nsk);
      }
      else
      {
        JmPushRet(before_call);
      }
      break;

    default:
      JmPushRet(before_call);
      break;
    }

    JmPushRet( after_call );
}
#endif

/*$Id: shvec.c,v 1.31 1999/10/13 20:37:08 bsmith Exp bsmith $*/

/*
   This file contains routines for Parallel vector operations that use shared memory
 */
#include "src/vec/impls/mpi/pvecimpl.h"   /*I  "vec.h"   I*/

/*
     Could not get the include files to work properly on the SGI with 
  the C++ compiler.
*/
#if defined(PETSC_USE_SHARED_MEMORY) && !defined(__cplusplus)

extern void *PetscSharedMalloc(int,int,MPI_Comm);

#undef __FUNC__  
#define __FUNC__ "VecDuplicate_Shared"
int VecDuplicate_Shared( Vec win, Vec *v)
{
  int     ierr;
  Vec_MPI *vw, *w = (Vec_MPI *)win->data;
  Scalar  *array;

  PetscFunctionBegin;

  /* first processor allocates entire array and sends it's address to the others */
  array = (Scalar *) PetscSharedMalloc(win->n*sizeof(Scalar),win->N*sizeof(Scalar),win->comm);CHKPTRQ(array);

  ierr = VecCreate(win->comm,win->n,win->N,v);CHKERRQ(ierr);
  ierr = VecCreate_MPI_Private(*v,w->nghost,array,win->map);CHKERRQ(ierr);
  vw   = (Vec_MPI *)(*v)->data;

  /* New vector should inherit stashing property of parent */
  vw->donotstash = w->donotstash;
  
  ierr = OListDuplicate(win->olist,&(*v)->olist);CHKERRQ(ierr);
  ierr = FListDuplicate(win->qlist,&(*v)->qlist);CHKERRQ(ierr);

  if (win->mapping) {
    (*v)->mapping = win->mapping;
    PetscObjectReference((PetscObject)win->mapping);
  }
  (*v)->ops->duplicate = VecDuplicate_Shared;
  PetscFunctionReturn(0);
}


EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "VecCreate_Shared"
int VecCreate_Shared(Vec vv)
{
  int     ierr;
  Scalar  *array;

  PetscFunctionBegin;
  ierr = PetscSplitOwnership(vv->comm,&vv->n,&vv->N);CHKERRQ(ierr);
  array = (Scalar *) PetscSharedMalloc(vv->n*sizeof(Scalar),vv->N*sizeof(Scalar),vv->comm);CHKPTRQ(array); 

  ierr = VecCreate_MPI_Private(vv,0,array,PETSC_NULL);CHKERRQ(ierr);
  vv->ops->duplicate = VecDuplicate_Shared;

  PetscFunctionReturn(0);
}
EXTERN_C_END


/* ----------------------------------------------------------------------------------------
     Code to manage shared memory allocation under the SGI with MPI

  We associate with a communicator a shared memory "areana" from which memory may be shmalloced.
*/
#include "sys.h"
#include "pinclude/ptime.h"
#if defined(PETSC_HAVE_PWD_H)
#include <pwd.h>
#endif
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#if defined(PETSC_HAVE_UNISTD_H)
#include <unistd.h>
#endif
#if defined(PETSC_HAVE_STDLIB_H)
#include <stdlib.h>
#endif
#if !defined(PARCH_win32)
#include <sys/param.h>
#include <sys/utsname.h>
#endif
#if defined(PARCH_win32)
#include <windows.h>
#include <io.h>
#include <direct.h>
#endif
#if defined (PARCH_win32_gnu)
#include <windows.h>
#endif
#include <fcntl.h>
#include <time.h>  
#if defined(PETSC_HAVE_SYS_SYSTEMINFO_H)
#include <sys/systeminfo.h>
#endif
#include "pinclude/petscfix.h"

static int Petsc_Shared_keyval = MPI_KEYVAL_INVALID;
static int Petsc_Shared_size   = 100000000;

#undef __FUNC__  
#define __FUNC__ "Petsc_DeleteShared" 
/*
   Private routine to delete internal storage when a communicator is freed.
  This is called by MPI, not by users.

  The binding for the first argument changed from MPI 1.0 to 1.1; in 1.0
  it was MPI_Comm *comm.  
*/
static int Petsc_DeleteShared(MPI_Comm comm,int keyval,void* attr_val,void* extra_state )
{
  int ierr;

  PetscFunctionBegin;
  ierr = PetscFree(attr_val);CHKERRQ(ierr);
  PetscFunctionReturn(MPI_SUCCESS);
}

#undef __FUNC__  
#define __FUNC__ "PetscSharedMemorySetSize"
int PetscSharedMemorySetSize(int s)
{
  PetscFunctionBegin;
  Petsc_Shared_size = s;
  PetscFunctionReturn(0);
}

#include "pinclude/petscfix.h"

#include <ulocks.h>

#undef __FUNC__  
#define __FUNC__ "PetscSharedInitialize"
int PetscSharedInitialize(MPI_Comm comm)
{
  int     rank,len,ierr,flag;
  char    filename[256];
  usptr_t **arena;

  PetscFunctionBegin;

  if (Petsc_Shared_keyval == MPI_KEYVAL_INVALID) {
    /* 
       The calling sequence of the 2nd argument to this function changed
       between MPI Standard 1.0 and the revisions 1.1 Here we match the 
       new standard, if you are using an MPI implementation that uses 
       the older version you will get a warning message about the next line;
       it is only a warning message and should do no harm.
    */
    ierr = MPI_Keyval_create(MPI_NULL_COPY_FN,Petsc_DeleteShared,&Petsc_Shared_keyval,0);CHKERRQ(ierr);
  }

  ierr = MPI_Attr_get(comm,Petsc_Shared_keyval,(void**)&arena,&flag);CHKERRQ(ierr);

  if (!flag) {
    /* This communicator does not yet have a shared memory areana */
    arena    = (usptr_t**) PetscMalloc( sizeof(usptr_t*) );CHKPTRQ(arena);

    ierr = MPI_Comm_rank(comm,&rank);CHKERRQ(ierr);
    if (!rank) {
      ierr = PetscStrcpy(filename,"/tmp/PETScArenaXXXXXX");CHKERRQ(ierr);
      mktemp(filename);
      ierr = PetscStrlen(filename,&len);CHKERRQ(ierr);
    } 
    ierr     = MPI_Bcast(&len,1,MPI_INT,0,comm);CHKERRQ(ierr);
    ierr     = MPI_Bcast(filename,len+1,MPI_CHAR,0,comm);CHKERRQ(ierr);
    ierr     = OptionsGetInt(PETSC_NULL,"-shared_size",&Petsc_Shared_size,&flag);CHKERRQ(ierr);
    usconfig(CONF_INITSIZE,Petsc_Shared_size);
    *arena   = usinit(filename); 
    ierr     = MPI_Attr_put(comm,Petsc_Shared_keyval, arena);CHKERRQ(ierr);
  } 

  PetscFunctionReturn(0);
}

#undef __FUNC__  
#define __FUNC__ "PetscSharedMalloc"
void *PetscSharedMalloc(int llen,int len,MPI_Comm comm)
{
  char    *value;
  int     ierr,shift,rank,flag;
  usptr_t **arena;

  PetscFunctionBegin;
  if (Petsc_Shared_keyval == MPI_KEYVAL_INVALID) {
    ierr = PetscSharedInitialize(comm); 
    if (ierr) PetscFunctionReturn(0);
  }
  ierr = MPI_Attr_get(comm,Petsc_Shared_keyval,(void**)&arena,&flag);
  if (ierr) PetscFunctionReturn(0);
  if (!flag) { 
    ierr = PetscSharedInitialize(comm);
    if (ierr) {PetscFunctionReturn(0);}
    ierr = MPI_Attr_get(comm,Petsc_Shared_keyval,(void**)&arena,&flag);
    if (ierr) PetscFunctionReturn(0);
  } 

  ierr   = MPI_Scan(&llen,&shift,1,MPI_INT,MPI_SUM,comm); if (ierr) PetscFunctionReturn(0);
  shift -= llen;

  ierr = MPI_Comm_rank(comm,&rank); if (ierr) PetscFunctionReturn(0);
  if (!rank) {
    value = (char *) usmalloc((size_t) len, *arena);
    if (!value) {
      (*PetscErrorPrintf)("PETSC ERROR: Unable to allocate shared memory location\n");
      (*PetscErrorPrintf)("PETSC ERROR: Run with option -shared_size <size> \n");
      (*PetscErrorPrintf)("PETSC_ERROR: with size > %d \n",(int)(1.2*(Petsc_Shared_size+len)));
      PetscError(__LINE__,__FUNC__,__FILE__,__SDIR__,1,1,"Unable to malloc shared memory");
      PetscFunctionReturn(0);
    }
  }
  ierr = MPI_Bcast(&value,8,MPI_BYTE,0,comm); if (ierr) PetscFunctionReturn(0);
  value += shift; 

  PetscFunctionReturn((void *)value);
}

#else

EXTERN_C_BEGIN
#undef __FUNC__  
#define __FUNC__ "VecCreate_Shared"
int VecCreate_Shared(MPI_Comm comm,int n,int N,Vec *vv)
{
  int ierr,size;

  PetscFunctionBegin;
  ierr = MPI_Comm_size(comm,&size);CHKERRQ(ierr);
  if (size > 1) {
    SETERRQ(1,1,"No supported for shared memory vector objects on this machine");
  }
  ierr = VecCreateSeq(comm,PetscMax(n,N),vv);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
EXTERN_C_END

#endif

#undef __FUNC__  
#define __FUNC__ "VecCreateShared"
/*@C
   VecCreateShared - Creates a parallel vector that uses shared memory.

   Input Parameters:
.  comm - the MPI communicator to use
.  n - local vector length (or PETSC_DECIDE to have calculated if N is given)
.  N - global vector length (or PETSC_DECIDE to have calculated if n is given)

   Output Parameter:
.  vv - the vector

   Collective on MPI_Comm
 
   Notes:
   Currently VecCreateShared() is available only on the SGI; otherwise,
   this routine is the same as VecCreateMPI().

   Use VecDuplicate() or VecDuplicateVecs() to form additional vectors of the
   same type as an existing vector.

   Level: advanced

.keywords: vector, create, shared

.seealso: VecCreateSeq(), VecCreate(), VecCreateMPI(), VecDuplicate(), VecDuplicateVecs(), 
          VecCreateGhost(), VecCreateMPIWithArray(), VecCreateGhostWithArray()

@*/ 
int VecCreateShared(MPI_Comm comm, int n, int N, Vec *v)
{
  int ierr;

  PetscFunctionBegin;
  ierr = VecCreate(comm,n,N,v);CHKERRQ(ierr);
  ierr = VecSetType(*v,VEC_SHARED);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}






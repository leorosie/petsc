/*$Id: daload.c,v 1.12 1999/10/01 21:23:00 bsmith Exp bsmith $*/

#include "src/dm/da/daimpl.h"     /*I  "da.h"   I*/


#undef __FUNC__  
#define __FUNC__ "DALoad"
/*@C
      DALoad - Creates an appropriate DA and loads its global vector from a file.

   Input Parameter:
+    viewer - a binary viewer (created with ViewerBinaryOpen())
.    M - number of processors in x direction
.    N - number of processors in y direction
-    P - number of processors in z direction

   Output Parameter:
.    da - the DA object

   Level: intermediate

@*/
int DALoad(Viewer viewer,int M,int N, int P,DA *da)
{
  int        ierr,info[8],nmax = 8,flag,fd,i;
  MPI_Comm   comm;
  char       fieldnametag[32],fieldname[64];
  PetscTruth isbinary;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(viewer,VIEWER_COOKIE);
  PetscValidPointer(da);
  ierr = PetscTypeCompare((PetscObject)viewer,BINARY_VIEWER,&isbinary);CHKERRQ(ierr);
  if (!isbinary) SETERRQ(PETSC_ERR_ARG_WRONG,0,"Must be binary viewer");

  ierr = ViewerBinaryGetDescriptor(viewer,&fd);CHKERRQ(ierr);
  ierr = PetscObjectGetComm((PetscObject)viewer,&comm);CHKERRQ(ierr);

  ierr = OptionsGetIntArray(PETSC_NULL,"-daload_info",info,&nmax,&flag);CHKERRQ(ierr);
  if (!flag) {
    SETERRQ(1,1,"No DA information in file");
  }
  if (nmax != 8) {
    SETERRQ1(1,1,"Wrong number of items in DA information in file: %d",nmax);
  }
  if (info[0] == 1) {
    ierr = DACreate1d(comm,(DAPeriodicType) info[7],info[1],info[4],info[5],0,da);CHKERRQ(ierr);
  } else if (info[0] == 2) {
    ierr = DACreate2d(comm,(DAPeriodicType) info[7],(DAStencilType) info[6],info[1],info[2],M,N,info[4],
                      info[5],0,0,da);CHKERRQ(ierr);
  } else if (info[0] == 3) {
    ierr = DACreate3d(comm,(DAPeriodicType) info[7],(DAStencilType) info[6],info[1],info[2],info[3],M,N,P,
                      info[4],info[5],0,0,0,da);CHKERRQ(ierr);
  } else {
    SETERRQ1(1,1,"Dimension in info file is not 1, 2, or 3 it is %d",info[0]);
  }
  for ( i=0; i<info[4]; i++ ) {
    sprintf(fieldnametag,"-daload_fieldname_%d",i);
    ierr = OptionsGetString(PETSC_NULL,fieldnametag,fieldname,64,&flag);CHKERRQ(ierr);
    if (flag) {
      ierr = DASetFieldName(*da,i,fieldname);CHKERRQ(ierr);
    }
  }

  /*
    Read in coordinate information if kept in file
  */
  ierr = OptionsHasName(PETSC_NULL,"-daload_coordinates",&flag);CHKERRQ(ierr);
  if (flag) {
    DA  dac;
    Vec natural,global;
    int mlocal;

    if (info[0] == 1) {
      ierr = DACreate1d(comm,DA_NONPERIODIC,info[1],1,0,0,&dac);CHKERRQ(ierr);
    } else if (info[0] == 2) {
      ierr = DACreate2d(comm,DA_NONPERIODIC,DA_STENCIL_BOX,info[1],info[2],M,N,2,
                      0,0,0,&dac);CHKERRQ(ierr);
    } else if (info[0] == 3) {
      ierr = DACreate3d(comm,DA_NONPERIODIC,DA_STENCIL_BOX,info[1],info[2],info[3],M,N,P,
                        3,0,0,0,0,&dac);CHKERRQ(ierr);
    }
    ierr = DACreateNaturalVector(dac,&natural);CHKERRQ(ierr);
    ierr = VecLoadIntoVector(viewer,natural);CHKERRQ(ierr);
    ierr = VecGetLocalSize(natural,&mlocal);CHKERRQ(ierr);
    ierr = VecCreateMPI(comm,mlocal,PETSC_DETERMINE,&global);CHKERRQ(ierr);
    ierr = DANaturalToGlobalBegin(dac,natural,INSERT_VALUES,global);CHKERRQ(ierr);
    ierr = DANaturalToGlobalEnd(dac,natural,INSERT_VALUES,global);CHKERRQ(ierr);
    ierr = VecDestroy(natural);CHKERRQ(ierr); 
    ierr = DADestroy(dac);CHKERRQ(ierr);
    ierr = DASetCoordinates(*da,global);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

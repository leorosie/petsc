/*$Id: closeport.c,v 1.13 1999/06/30 22:48:50 bsmith Exp bsmith $*/
/* This was part of the MatlabSockettool package. 
 
        Written by Barry Smith, bsmith@mcs.anl.gov 4/14/92
*/

#include "petsc.h"
#if defined(PETSC_NEEDS_UTYPE_TYPEDEFS)
typedef unsigned char   u_char;
typedef unsigned short  u_short;
typedef unsigned short  ushort;
typedef unsigned int    u_int;
typedef unsigned long   u_long;
#endif
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include "src/sys/src/viewer/impls/socket/socket.h"
#include "mex.h"
#define ERROR(a) {fprintf(stderr,"CLOSEPORT: %s \n",a); return ;}
typedef struct { int onoff; int time; } Linger;
/*-----------------------------------------------------------------*/
/*                                                                 */
/*-----------------------------------------------------------------*/
#undef __FUNC__  
#define __FUNC__ "mexFunction"
void mexFunction(int nlhs, Matrix *plhs[], int nrhs, Matrix *prhs[])
{
  int    t = 0;
  Linger linger;

  linger.onoff = 1;
  linger.time  = 0; 

  if (nrhs == 0) ERROR("Needs one argument, the port");
  t = (int) *mxGetPr(prhs[0]);

  if (setsockopt(t,SOL_SOCKET,SO_LINGER,(char *) &linger,sizeof(Linger))) 
    ERROR("Setting linger");
  if (close(t)) ERROR("closing socket");
  return;
}

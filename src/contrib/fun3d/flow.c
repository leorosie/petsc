/* "$Id: flow.c,v 1.43 2000/08/01 22:04:24 kaushik Exp bsmith $";*/

static char help[] = "FUN3D - 3-D, Unstructured Incompressible Euler Solver\n\
originally written by W. K. Anderson of NASA Langley, \n\
and ported into PETSc by D. K. Kaushik, ODU and ICASE.\n\n";

#include "petscsnes.h"
#include "petscao.h"
#include "user.h"

#define ICALLOC(size,y) *(y) = (int*)PetscMalloc((PetscMax(size,1))*sizeof(int));CHKPTRQ(*(y))
#define FCALLOC(size,y) *(y) = (Scalar*)PetscMalloc((PetscMax(size,1))*sizeof(Scalar));CHKPTRQ(*(y))
 
typedef struct {
 Vec     qnew,qold,func;
 double  fnorm_ini,dt_ini,cfl_ini;
 double  ptime;
 double  cfl_max,max_time;
 double  fnorm,dt,cfl;
 double  fnorm_ratio;
 int     ires,iramp;
 int     max_steps,print_freq;
} TstepCtx;
 
typedef struct {                               /*============================*/
 GRID        *grid;                               /* Pointer to Grid info       */
 TstepCtx    *tsCtx;
 PetscTruth  PreLoading;                          /* Pointer to Time Stepping Context */
} AppCtx;                                      /*============================*/

extern int  FormJacobian(SNES,Vec,Mat*,Mat*,MatStructure*,void*),
            FormFunction(SNES,Vec,Vec,void*),
            FormInitialGuess(SNES,GRID *),
            Update(SNES,void*),
            ComputeTimeStep(SNES,int,void*),
            GetLocalOrdering(GRID *),
            SetPetscDS(GRID *,TstepCtx *);

/* Global Variables */ 

                                               /*============================*/
CINFO  *c_info;                                /* Pointer to COMMON INFO     */
CRUNGE *c_runge;                               /* Pointer to COMMON RUNGE    */
CGMCOM *c_gmcom;                               /* Pointer to COMMON GMCOM    */
                                               /*============================*/
int  rank,size,rstart;
REAL memSize = 0.0,grad_time = 0.0;
#if defined(_OPENMP)
int max_threads = 2;
#endif

#if defined(PARCH_IRIX64) && defined(USE_HW_COUNTERS)
int       event0,event1;
Scalar    time_counters;
long long counter0,counter1;
#endif
int  ntran[max_nbtran];        /* transition stuff put here to make global */
REAL dxtran[max_nbtran];

#ifdef PETSC_HAVE_AMS_no
AMS_Comm ams;
AMS_Memory memid;
int ams_err;
char *msg;
char *vtk = "vtk", *grid_type = "Unstructured Grid", *cell_type = "VTK_TRIANGLE";
int point_dims[2], cell_dims[2];
int p_start_ind[2], p_end_ind[2];
int c_start_ind[2], c_end_ind[2];
int s_start_ind[2], s_end_ind[2];
#endif
 
/* ======================== MAIN ROUTINE =================================== */
/*                                                                           */
/* Finite volume flux split solver for general polygons                      */
/*                                                                           */
/*===========================================================================*/

#undef __FUNC__
#define __FUNC__ "main"
int main(int argc,char **args)
{
  AppCtx 	user;
  GRID 		f_pntr;
  TstepCtx      tsCtx ;
  SNES          snes;                  /* nonlinear solver context */
  Mat           Jpc;                   /* Jacobian and Preconditioner matrices */
  Scalar        *qnode;
  int 		ierr,ileast;
  PetscTruth    flg;
  
  ierr = PetscInitialize(&argc,&args,"testgrid/petsc.opt",help);CHKERRQ(ierr);
  ierr = PetscInitializeFortran();CHKERRQ(ierr);

  ierr = MPI_Comm_rank(PETSC_COMM_WORLD,&rank);CHKERRQ(ierr);
  ierr = MPI_Comm_size(PETSC_COMM_WORLD,&size);CHKERRQ(ierr);

  
  /*======================================================================*/
  /* Initilize stuff related to time stepping */
  /*======================================================================*/
  tsCtx.fnorm_ini = 0.0;  tsCtx.cfl_ini     = 50.0;    tsCtx.cfl_max = 1.0e+05;
  tsCtx.max_steps = 50;   tsCtx.max_time    = 1.0e+12; tsCtx.iramp   = -50;
  tsCtx.dt        = -5.0; tsCtx.fnorm_ratio = 1.0e+10;
  ierr = OptionsGetInt(PETSC_NULL,"-max_st",&tsCtx.max_steps,PETSC_NULL);CHKERRQ(ierr);
  ierr = OptionsGetDouble(PETSC_NULL,"-ts_rtol",&tsCtx.fnorm_ratio,PETSC_NULL);CHKERRQ(ierr);
  ierr = OptionsGetDouble(PETSC_NULL,"-cfl_ini",&tsCtx.cfl_ini,PETSC_NULL);CHKERRQ(ierr);
  ierr = OptionsGetDouble(PETSC_NULL,"-cfl_max",&tsCtx.cfl_max,PETSC_NULL);CHKERRQ(ierr);
  /*======================================================================*/

  /*Set the maximum number of threads for OpenMP */
#if defined(_OPENMP)
  ierr = OptionsGetInt(PETSC_NULL,"-max_threads",&max_threads,&flg);CHKERRQ(ierr);
  omp_set_num_threads(max_threads);
  ierr = PetscPrintf(PETSC_COMM_WORLD,"Using %d threads for each MPI process\n",max_threads);CHKERRQ(ierr);
#endif
  f77FORLINK();                               /* Link FORTRAN and C COMMONS */
 
  f77OPENM(&rank);                            /* Open files for I/O         */

  /*  Read input */

  f77READR1(&ileast,&rank);
  f_pntr.jvisc   = c_info->ivisc;
  f_pntr.ileast  = ileast;
  c_gmcom->ilu0  = 1;
  c_gmcom->nsrch = 10;   
 
  c_runge->nitfo = 0;

  /* Get the grid information into local ordering */
  ierr = GetLocalOrdering(&f_pntr);CHKERRQ(ierr);

  /* Allocate Memory for Some Other Grid Arrays */ 
  ierr = set_up_grid(&f_pntr);CHKERRQ(ierr);
 
  /* If using least squares for the gradients,calculate the r's */
  if (f_pntr.ileast == 4) {
    f77SUMGS(&f_pntr.nnodesLoc,&f_pntr.nedgeLoc,f_pntr.eptr,f_pntr.xyz,f_pntr.rxy,&rank,&f_pntr.nvertices);
  }
 
  user.grid  = &f_pntr;
  user.tsCtx = &tsCtx;

    /* AMS Stuff */
#ifdef PETSC_HAVE_AMS_no
    /* Create and publish the Communicator */
    ams_err = AMS_Comm_publish("FUN3D",&ams,MPI_TYPE);
    AMS_Check_error(ams_err,&msg);
  
    /* Create a Memory */
    ams_err = AMS_Memory_create(ams,"FUN3D-MEM",&memid);
    AMS_Check_error(ams_err,&msg);

    /* Add vtk fields to the memory */
    ams_err =  AMS_Memory_add_field(memid,"vtk",&vtk,1,AMS_STRING,
                               AMS_READ,AMS_COMMON,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Add type field */
    ams_err =  AMS_Memory_add_field(memid,"type",&grid_type,1,AMS_STRING,
                               AMS_READ,AMS_COMMON,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Add point dimensions field */
    point_dims[0] = user.grid->nnodes;
    point_dims[1] = 3;

    ams_err =  AMS_Memory_add_field(memid,"point dims",point_dims,2,AMS_INT,
                               AMS_READ,AMS_COMMON,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Add cell dimensions field */
    
    cell_dims[0] = user.grid->ncell;
    cell_dims[1] = 3;

    ams_err =  AMS_Memory_add_field(memid,"cell dims",cell_dims,2,AMS_INT,
                               AMS_READ,AMS_COMMON,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Add cell type field */
    ams_err =  AMS_Memory_add_field(memid,"cell type",&cell_type,1,AMS_STRING,
                               AMS_READ,AMS_COMMON,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Add points field */
    ams_err =  AMS_Memory_add_field(memid,"points",user.grid->xyz,3*user.grid->nnodesLoc,
                          AMS_DOUBLE,AMS_READ,AMS_DISTRIBUTED,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Set Field Dimensions */
    p_start_ind[0] = rstart;   /* Starting index in the first dimension */
    p_end_ind[0] = rstart+user.grid->nnodesLoc-1;    /* Ending index in the first dimension */

    p_start_ind[1] = 0;   /* Starting index in the second dimension */
    p_end_ind[1] = 2;    /* Ending index in the second dimension */

    /*
     * This would be an array of user.grid->nnodesLoc rows and 3 columns
     */ 
    ams_err = AMS_Memory_set_field_block(memid,"points",2,p_start_ind,p_end_ind);
    AMS_Check_error(ams_err,&msg);

    /* Add cells field */
    ams_err =  AMS_Memory_add_field(memid,"cells",user.grid->nntet,4*user.grid->ncellLoc,
                          AMS_INT,AMS_READ,AMS_DISTRIBUTED,AMS_REDUCT_UNDEF); 
    AMS_Check_error(ams_err,&msg);

    /* Set Field Dimensions */
    c_start_ind[0] = rstart;   /* Starting index in the first dimension */
    c_end_ind[0] = rstart+user.grid->ncellLoc-1;  /* Ending index in the first dimension */

    c_start_ind[1] = 0;   /* Starting index in the second dimension */
    c_end_ind[1] = 3;    /* Ending index in the second dimension */

    /*
     * This would be an array of user.grid->nnodesLoc rows and 3 columns
     */ 
    ams_err = AMS_Memory_set_field_block(memid,"cells",2,c_start_ind,c_end_ind);
    AMS_Check_error(ams_err,&msg);


    /* Add points field */
    ams_err =  AMS_Memory_add_field(memid,"supervectors",user.grid->qnode,4*user.grid->nnodesLoc,
                          AMS_DOUBLE,AMS_READ,AMS_DISTRIBUTED,AMS_REDUCT_UNDEF);
    AMS_Check_error(ams_err,&msg);

    /* Set Field Dimensions */
    s_start_ind[0] = rstart;   /* Starting index in the first dimension */
    s_end_ind[0] = rstart+user.grid->nnodesLoc-1;    /* Ending index in the first dimension */

    s_start_ind[1] = 0;   /* Starting index in the second dimension */
    s_end_ind[1] = 3;    /* Ending index in the second dimension */

    /*
     * This would be an array of ??? rows and 4 columns
     */ 
    ams_err = AMS_Memory_set_field_block(memid,"supervectors",2,s_start_ind,s_end_ind);
    AMS_Check_error(ams_err,&msg);

#endif

    /* 
     Preload the executable to get accurate timings. This runs the following chunk of 
     code twice, first to get the executable pages into memory and the second time for
     accurate timings.
    */
    PreLoadBegin(PETSC_TRUE,"Time integration");
    user.PreLoading = PreLoading;

    /* Create nonlinear solver */
    ierr = SetPetscDS(&f_pntr,&tsCtx);CHKERRQ(ierr);
    ierr = SNESCreate(PETSC_COMM_WORLD,SNES_NONLINEAR_EQUATIONS,&snes);CHKERRQ(ierr);
    ierr = SNESSetType(snes,"ls");CHKERRQ(ierr);
 
    /* Set various routines and options */
    ierr = SNESSetFunction(snes,user.grid->res,FormFunction,&user);CHKERRQ(ierr);
    ierr = OptionsHasName(PETSC_NULL,"-matrix_free",&flg);CHKERRQ(ierr);
    if (flg) {
      /* Use matrix-free to define Newton system; use explicit (approx) Jacobian for preconditioner */
      ierr = MatCreateSNESMF(snes,user.grid->qnode,&Jpc);CHKERRQ(ierr);
      ierr = SNESSetJacobian(snes,Jpc,user.grid->A,FormJacobian,&user);CHKERRQ(ierr);
    } else {
      /* Use explicit (approx) Jacobian to define Newton system and preconditioner */
      ierr = SNESSetJacobian(snes,user.grid->A,user.grid->A,FormJacobian,&user);CHKERRQ(ierr);
    }
 
    ierr = SNESSetFromOptions(snes);CHKERRQ(ierr);
 
    /* Initialize the flowfield */
    ierr = FormInitialGuess(snes,user.grid);CHKERRQ(ierr);

    /* Solve nonlinear system */
    ierr = Update(snes,&user);CHKERRQ(ierr);

    /* Write restart file */
    ierr = VecGetArray(user.grid->qnode,&qnode);CHKERRQ(ierr);
    /*f77WREST(&user.grid->nnodes,qnode,user.grid->turbre,user.grid->amut);*/

    /* Write Tecplot solution file */
    /*
    if (!rank) 
    f77TECFLO(&user.grid->nnodes, 
    &user.grid->nnbound,&user.grid->nvbound,&user.grid->nfbound,
    &user.grid->nnfacet,&user.grid->nvfacet,&user.grid->nffacet,
    &user.grid->nsnode, &user.grid->nvnode, &user.grid->nfnode,  
    c_info->title,     
    user.grid->x,       user.grid->y,       user.grid->z,
    qnode,
    user.grid->nnpts,   user.grid->nntet,   user.grid->nvpts,
    user.grid->nvtet,   user.grid->nfpts,   user.grid->nftet,   
    user.grid->f2ntn,   user.grid->f2ntv,   user.grid->f2ntf,
    user.grid->isnode,  user.grid->ivnode,  user.grid->ifnode,
    &rank); 
    */

    /*f77FASFLO(&user.grid->nnodes,&user.grid->nsnode,&user.grid->nnfacet,
    user.grid->isnode, user.grid->f2ntn,
    user.grid->x,      user.grid->y,      user.grid->z,
    qnode);*/

    /* Write residual,lift,drag,and moment history file */
    /*
     if (!rank) f77PLLAN(&user.grid->nnodes,&rank);
    */

    ierr = VecRestoreArray(user.grid->qnode,&qnode);CHKERRQ(ierr);
    ierr = OptionsHasName(PETSC_NULL,"-mem_use",&flg);CHKERRQ(ierr);
    if (flg) {
      ierr = PetscShowMemoryUsage(VIEWER_STDOUT_WORLD,"Memory usage before destroying\n");CHKERRQ(ierr);
    }

    ierr = VecDestroy(user.grid->qnode);CHKERRQ(ierr);
    ierr = VecDestroy(user.grid->qnodeLoc);CHKERRQ(ierr);
    ierr = VecDestroy(user.tsCtx->qold);CHKERRQ(ierr);
    ierr = VecDestroy(user.tsCtx->func);CHKERRQ(ierr);
    ierr = VecDestroy(user.grid->res);CHKERRQ(ierr);
    ierr = VecDestroy(user.grid->grad);CHKERRQ(ierr);
    ierr = VecDestroy(user.grid->gradLoc);CHKERRQ(ierr);
    ierr = MatDestroy(user.grid->A);CHKERRQ(ierr);
    ierr = OptionsHasName(PETSC_NULL,"-matrix_free",&flg);CHKERRQ(ierr);
    if (flg) { ierr = MatDestroy(Jpc);CHKERRQ(ierr);}
    ierr = SNESDestroy(snes);CHKERRQ(ierr);
    ierr = VecScatterDestroy(user.grid->scatter);CHKERRQ(ierr);
    ierr = VecScatterDestroy(user.grid->gradScatter);CHKERRQ(ierr);
    ierr = OptionsHasName(PETSC_NULL,"-mem_use",&flg);CHKERRQ(ierr);
    if (flg) {
      ierr = PetscShowMemoryUsage(VIEWER_STDOUT_WORLD,"Memory usage after destroying\n");CHKERRQ(ierr);
    }
  PreLoadEnd();

  ierr = PetscPrintf(PETSC_COMM_WORLD,"Time taken in gradient calculation %g sec.\n",grad_time);CHKERRQ(ierr);

  PetscFinalize();
  return 0;
}

/*---------------------------------------------------------------------*/
/* ---------------------  Form initial approximation ----------------- */
#undef __FUNC__
#define __FUNC__ "FormInitialGuess"
int FormInitialGuess(SNES snes,GRID *grid)
/*---------------------------------------------------------------------*/
{
  int    ierr;
  Scalar *qnode;

  PetscFunctionBegin;
  ierr = VecGetArray(grid->qnode,&qnode);CHKERRQ(ierr);
  f77INIT(&grid->nnodesLoc,qnode,grid->turbre,grid->amut,&grid->nvnodeLoc,grid->ivnode,&rank);
  ierr = VecRestoreArray(grid->qnode,&qnode);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}
 
/*---------------------------------------------------------------------*/
/* ---------------------  Evaluate Function F(x) --------------------- */
#undef __FUNC__
#define __FUNC__ "FormFunction"
int FormFunction(SNES snes,Vec x,Vec f,void *dummy)
/*---------------------------------------------------------------------*/
{
   AppCtx       *user = (AppCtx *) dummy;
   GRID         *grid = user->grid;
   TstepCtx     *tsCtx = user->tsCtx;
   Scalar       *qnode,*res,*qold;
   Scalar       *grad;
   Scalar       temp;
   VecScatter   scatter = grid->scatter;
   VecScatter   gradScatter = grid->gradScatter;
   Vec          localX = grid->qnodeLoc;
   Vec          localGrad = grid->gradLoc;
   int          i,j,in,ierr;
   int          nbface,ires;
   Scalar	time_ini,time_fin;
 
   PetscFunctionBegin;
   /* Get X into the local work vector */
   ierr = VecScatterBegin(x,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);
   ierr = VecScatterEnd(x,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);
   
   /* access the local work f,grad,and input */
   ierr = VecGetArray(f,&res);CHKERRQ(ierr);
   ierr = VecGetArray(grid->grad,&grad);CHKERRQ(ierr);
   ierr = VecGetArray(localX,&qnode);CHKERRQ(ierr);
   ires = tsCtx->ires;

   ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);
   f77LSTGS(&grid->nnodesLoc,&grid->nedgeLoc,grid->eptr,qnode,grad,grid->xyz,grid->rxy,
            &rank,&grid->nvertices);
   ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
   grad_time += time_fin - time_ini;
   ierr = VecRestoreArray(grid->grad,&grad);CHKERRQ(ierr);

   ierr = VecScatterBegin(grid->grad,localGrad,INSERT_VALUES,SCATTER_FORWARD,gradScatter);CHKERRQ(ierr);
   ierr = VecScatterEnd(grid->grad,localGrad,INSERT_VALUES,SCATTER_FORWARD,gradScatter);CHKERRQ(ierr);

   ierr = VecGetArray(localGrad,&grad);CHKERRQ(ierr);
   nbface = grid->nsface + grid->nvface + grid->nfface;
   f77GETRES(&grid->nnodesLoc,&grid->ncell,  &grid->nedgeLoc,  &grid->nsface,
             &grid->nvface,&grid->nfface, &nbface,
             &grid->nsnodeLoc,&grid->nvnodeLoc, &grid->nfnodeLoc,
              grid->isface, grid->ivface,  grid->ifface, &grid->ileast,
              grid->isnode, grid->ivnode,  grid->ifnode,
             &grid->nnfacetLoc,grid->f2ntn,  &grid->nnbound,
             &grid->nvfacetLoc,grid->f2ntv,  &grid->nvbound,
             &grid->nffacetLoc,grid->f2ntf,  &grid->nfbound,
              grid->eptr,
              grid->sxn,    grid->syn,     grid->szn,
              grid->vxn,    grid->vyn,     grid->vzn,
              grid->fxn,    grid->fyn,     grid->fzn,
              grid->xyzn,
              qnode,        grid->cdt,
              grid->xyz,    grid->area,
              grad,
              res,  grid->resd,         
              grid->turbre,
              grid->slen,   grid->c2n,
              grid->c2e,
              grid->us,     grid->vs,      grid->as,
              grid->phi,
              grid->amut,   &ires,&rank,&grid->nvertices);

/* Add the contribution due to time stepping */
  if (ires == 1) {
    ierr = VecGetArray(tsCtx->qold,&qold);CHKERRQ(ierr);
#if defined(INTERLACING)
    for (i = 0; i < grid->nnodesLoc; i++) {
     temp = grid->area[i]/(tsCtx->cfl*grid->cdt[i]);
     for (j = 0; j < 4; j++) {
      in       = 4*i + j;
      res[in] += temp*(qnode[in] - qold[in]);
     }
    }
#else
    for (j = 0; j < 4; j++) {
     for (i = 0; i < grid->nnodesLoc; i++) {
      temp     = grid->area[i]/(tsCtx->cfl*grid->cdt[i]);
      in       = grid->nnodesLoc*j + i;
      res[in] += temp*(qnode[in] - qold[in]);
     }
    }
#endif
    ierr = VecRestoreArray(tsCtx->qold,&qold);CHKERRQ(ierr);
  }
  ierr = VecRestoreArray(localX,&qnode);CHKERRQ(ierr);
  ierr = VecRestoreArray(f,&res);CHKERRQ(ierr);
  ierr = VecRestoreArray(localGrad,&grad);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

/*---------------------------------------------------------------------*/
/* --------------------  Evaluate Jacobian F'(x) -------------------- */
 
#undef __FUNC__
#define __FUNC__ "FormJacobian"
int FormJacobian(SNES snes,Vec x,Mat *Jac,Mat *B,MatStructure *flag,void *dummy)
/*---------------------------------------------------------------------*/
{
  AppCtx       *user = (AppCtx *) dummy;
  GRID         *grid = user->grid;
  TstepCtx     *tsCtx = user->tsCtx;
  Mat          jac = *B;
  VecScatter   scatter = grid->scatter;
  Vec          localX = grid->qnodeLoc;
  Scalar       *qnode;
  int          ierr;
 
  PetscFunctionBegin;
  ierr = VecScatterBegin(x,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);
  ierr = VecScatterEnd(x,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);
  ierr = MatSetUnfactored(jac);CHKERRQ(ierr); 
 
  ierr = VecGetArray(localX,&qnode);CHKERRQ(ierr);
  f77FILLA(&grid->nnodesLoc,&grid->nedgeLoc,grid->eptr,
            &grid->nsface,
             grid->isface,grid->fxn,grid->fyn,grid->fzn,
             grid->sxn,grid->syn,grid->szn,
            &grid->nsnodeLoc,&grid->nvnodeLoc,&grid->nfnodeLoc,grid->isnode,
             grid->ivnode,grid->ifnode,qnode,&jac,grid->cdt,
             grid->area,grid->xyzn,&tsCtx->cfl,
            &rank,&grid->nvertices);
  ierr = VecRestoreArray(localX,&qnode);CHKERRQ(ierr);
  *flag = SAME_NONZERO_PATTERN;
  PetscFunctionReturn(0);
}

/*---------------------------------------------------------------------*/
#undef __FUNC__
#define __FUNC__ "Update"
int Update(SNES snes,void *ctx)
/*---------------------------------------------------------------------*/
{
 
 AppCtx 	*user = (AppCtx *) ctx;
 GRID 		*grid = user->grid;
 TstepCtx 	*tsCtx = user->tsCtx;
 VecScatter   	scatter = grid->scatter;
 Vec          	localX = grid->qnodeLoc;
 Scalar 	*qnode,*res;
 Scalar 	clift,cdrag,cmom;
 int 		i,ierr,its;
 Scalar 	fratio;
 Scalar 	time1,time2,cpuloc,cpuglo;
 int 		max_steps;
 PetscTruth     print_flag = PETSC_FALSE;
 FILE 		*fptr = 0;
 int		nfailsCum = 0,nfails = 0;
 /*Scalar         cpu_ini,cpu_fin,cpu_time;*/
 /*int 		event0 = 14,event1 = 25,gen_start,gen_read;
 Scalar		time_start_counters,time_read_counters;
 long long      counter0,counter1;*/

  PetscFunctionBegin;

  ierr = OptionsHasName(PETSC_NULL,"-print",&print_flag);CHKERRQ(ierr);
  if (print_flag) {
    ierr = PetscFOpen(PETSC_COMM_WORLD,"history.out","w",&fptr);CHKERRQ(ierr);
    ierr = PetscFPrintf(PETSC_COMM_WORLD,fptr,"VARIABLES = iter,cfl,fnorm,clift,cdrag,cmom,cpu\n");CHKERRQ(ierr);
  }
  if (user->PreLoading) 
   max_steps = 1;
  else
   max_steps = tsCtx->max_steps;
  fratio = 1.0;
  /*tsCtx->ptime = 0.0;*/
  ierr = VecCopy(grid->qnode,tsCtx->qold);CHKERRQ(ierr);
  ierr = PetscGetTime(&time1);CHKERRQ(ierr);
#if defined (PARCH_IRIX64) && defined(USE_HW_COUNTERS)
 /*if (!user->PreLoading) {
  PetscTruth flg = PETSC_FALSE;
  ierr = OptionsGetInt(PETSC_NULL,"-e0",&event0,&flg);CHKERRQ(ierr);
  ierr = OptionsGetInt(PETSC_NULL,"-e1",&event1,&flg);CHKERRQ(ierr);
  ierr = PetscGetTime(&time_start_counters);CHKERRQ(ierr);
  if ((gen_start = start_counters(event0,event1)) < 0)
   SETERRQ(1,1,"Error in start_counters\n"); 
 }*/
#endif
 /*cpu_ini = PetscGetCPUTime();*/
 for (i = 0; i < max_steps && fratio <= tsCtx->fnorm_ratio; i++) {
  ierr = ComputeTimeStep(snes,i,user);CHKERRQ(ierr);
  /*tsCtx->ptime +=  tsCtx->dt;*/
  ierr = SNESSolve(snes,grid->qnode,&its);CHKERRQ(ierr);
  ierr = SNESGetNumberUnsuccessfulSteps(snes,&nfails);CHKERRQ(ierr);
  nfailsCum += nfails; nfails = 0;
  if (nfailsCum >= 2) SETERRQ(1,1,"Unable to find a Newton Step");
  if (print_flag){
    ierr = PetscPrintf(PETSC_COMM_WORLD,"At Time Step %d cfl = %g and fnorm = %g\n",i,tsCtx->cfl,tsCtx->fnorm);CHKERRQ(ierr);
  }
  ierr = VecCopy(grid->qnode,tsCtx->qold);CHKERRQ(ierr);

  c_info->ntt = i+1;
  ierr = PetscGetTime(&time2);CHKERRQ(ierr);
  cpuloc = time2-time1;            
  cpuglo = 0.0;
  ierr = MPI_Allreduce(&cpuloc,&cpuglo,1,MPI_DOUBLE,MPI_MAX,PETSC_COMM_WORLD);CHKERRQ(ierr);
  c_info->tot = cpuglo;    /* Total CPU time used upto this time step */
  
  ierr = VecScatterBegin(grid->qnode,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);
  ierr = VecScatterEnd(grid->qnode,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRQ(ierr);

  ierr = VecGetArray(grid->res,&res);CHKERRQ(ierr);
  ierr = VecGetArray(localX,&qnode);CHKERRQ(ierr);
  /*f77L2NORM(res,&grid->nnodesLoc,&grid->nnodes,grid->x,
            grid->y,   grid->z,
            grid->area,&rank);*/
  f77FORCE(&grid->nnodesLoc,&grid->nedgeLoc,  
            grid->isnode, grid->ivnode, 
           &grid->nnfacetLoc,grid->f2ntn,&grid->nnbound,
           &grid->nvfacetLoc,grid->f2ntv,&grid->nvbound,
            grid->eptr,   qnode,   
            grid->xyz,
            grid->sface_bit,grid->vface_bit,
            &clift,&cdrag,&cmom,&rank,&grid->nvertices);
  if (print_flag) {
    ierr = PetscPrintf(PETSC_COMM_WORLD,"%d\t%g\t%g\t%g\t%g\t%g\n",i,
                       tsCtx->cfl,tsCtx->fnorm,clift,cdrag,cmom);CHKERRQ(ierr);
    ierr = PetscPrintf(PETSC_COMM_WORLD,"Wall clock time needed %g seconds for %d time steps\n",
                       cpuglo,i);CHKERRQ(ierr);
    ierr = PetscFPrintf(PETSC_COMM_WORLD,fptr,"%d\t%g\t%g\t%g\t%g\t%g\t%g\n",
                        i,tsCtx->cfl,tsCtx->fnorm,clift,cdrag,cmom,cpuglo);
  }
  ierr = VecRestoreArray(localX,&qnode);CHKERRQ(ierr);
  ierr = VecRestoreArray(grid->res,&res);CHKERRQ(ierr);
  fratio = tsCtx->fnorm_ini/tsCtx->fnorm;
  ierr = MPI_Barrier(PETSC_COMM_WORLD);CHKERRQ(ierr);
 } /* End of time step loop */

#if defined (PARCH_IRIX64) && defined(USE_HW_COUNTERS)
 if (!user->PreLoading) {
  int eve0,eve1;
  FILE *cfp0,*cfp1;
  char str[256];
  /*if ((gen_read = read_counters(event0,&counter0,event1,&counter1)) < 0)
   SETERRQ(1,1,"Error in read_counter\n");
  ierr = PetscGetTime(&time_read_counters);CHKERRQ(ierr);
  if (gen_read != gen_start) {
   SETERRQ(1,1,"Lost Counters!! Aborting ...\n");
  }*/
  /*sprintf(str,"counters%d_and_%d",event0,event1);
  cfp0 = fopen(str,"a");*/
  /*ierr = print_counters(event0,counter0,event1,counter1);*/
  /*fprintf(cfp0,"%lld %lld %g\n",counter0,counter1,
                time_counters);
  fclose(cfp0);*/
 }
#endif
 ierr = PetscPrintf(PETSC_COMM_WORLD,"Total wall clock time needed %g seconds for %d time steps\n",
                    cpuglo,i);CHKERRQ(ierr);
 ierr = PetscPrintf(PETSC_COMM_WORLD,"cfl = %g fnorm = %g\n",tsCtx->cfl,tsCtx->fnorm);CHKERRQ(ierr);
 ierr = PetscPrintf(PETSC_COMM_WORLD,"clift = %g cdrag = %g cmom = %g\n",clift,cdrag,cmom);CHKERRQ(ierr);

 if (!rank && print_flag) fclose(fptr);
 if (user->PreLoading) {
   tsCtx->fnorm_ini = 0.0;
   ierr = PetscPrintf(PETSC_COMM_WORLD,"Preloading done ...\n");CHKERRQ(ierr);
 }

 PetscFunctionReturn(0);
}

/*---------------------------------------------------------------------*/
#undef __FUNC__
#define __FUNC__ "ComputeTimeStep"
int ComputeTimeStep(SNES snes,int iter,void *ctx)
/*---------------------------------------------------------------------*/
{
  AppCtx    *user = (AppCtx *) ctx;
  TstepCtx  *tsCtx = user->tsCtx;
  Vec	    func = tsCtx->func;
  Scalar    inc = 1.1;
  Scalar    newcfl;
  int       ierr;
  /*int	    iramp = tsCtx->iramp;*/
 
  PetscFunctionBegin;
 
  tsCtx->ires = 0;
  ierr = FormFunction(snes,tsCtx->qold,func,user);CHKERRQ(ierr);
  tsCtx->ires = 1;
  ierr = VecNorm(func,NORM_2,&tsCtx->fnorm);CHKERRQ(ierr);
  /* first time through so compute initial function norm */
  if (tsCtx->fnorm_ini == 0.0) {
    tsCtx->fnorm_ini = tsCtx->fnorm;
    tsCtx->cfl       = tsCtx->cfl_ini;
  } else {
     newcfl     = inc*tsCtx->cfl_ini*tsCtx->fnorm_ini/tsCtx->fnorm;
     tsCtx->cfl = PetscMin(newcfl,tsCtx->cfl_max);
  }
 
  /*if (iramp < 0) {
   newcfl = inc*tsCtx->cfl_ini*tsCtx->fnorm_ini/tsCtx->fnorm;
  } else {
   if (tsCtx->dt < 0 && iramp > 0)
    if (iter > iramp) newcfl = tsCtx->cfl_max;
    else newcfl = tsCtx->cfl_ini + (tsCtx->cfl_max - tsCtx->cfl_ini)*
                                (double) iter/(double) iramp;
  }
  tsCtx->cfl = MIN(newcfl,tsCtx->cfl_max);*/
  /*printf("In ComputeTime Step - fnorm is %f\n",tsCtx->fnorm);*/
  /*ierr = VecDestroy(func);CHKERRQ(ierr);*/
  PetscFunctionReturn(0);
}

/*---------------------------------------------------------------------*/
#undef __FUNC__
#define __FUNC__ "GetLocalOrdering"
int GetLocalOrdering(GRID *grid)
/*---------------------------------------------------------------------*/
{

  int        ierr,i,j,k,inode,isurf,nte,nb,node1,node2,node3;
  int 	     nnodes,nedge,nnz,jstart,jend;
  int	     nnodesLoc,nvertices,nedgeLoc,nnodesLocEst;
  int        nedgeLocEst,remEdges,readEdges,remNodes,readNodes;
  int 	     nnfacet,nvfacet,nffacet;
  int 	     nnfacetLoc,nvfacetLoc,nffacetLoc;
  int	     nsnode,nvnode,nfnode;
  int	     nsnodeLoc,nvnodeLoc,nfnodeLoc;
  int        nnbound,nvbound,nfbound;
  int        fdes,currentPos = 0,newPos = 0;
  int        grid_param = 13;
  int        *edge_bit,*pordering;
  int	     *l2p,*l2a,*p2l,*a2l,*v2p,*eperm;
  int	     *tmp,*tmp1,*tmp2;
  Scalar     time_ini,time_fin;
  Scalar     *ftmp,*ftmp1;
  char       mesh_file[256];
  AO         ao;
  FILE       *fptr,*fptr1;
  PetscTruth flg;
  MPI_Comm   comm = PETSC_COMM_WORLD;

  PetscFunctionBegin;
  /* Read the integer grid parameters */ 
  ICALLOC(grid_param,&tmp);
  if (!rank) {
   ierr = OptionsGetString(PETSC_NULL,"-mesh",mesh_file,256,&flg);CHKERRQ(ierr);
   ierr = PetscBinaryOpen(mesh_file,BINARY_RDONLY,&fdes);CHKERRQ(ierr);
  }
  ierr = PetscSynchronizedBinaryRead(comm,fdes,tmp,grid_param,PETSC_INT);CHKERRQ(ierr);
  grid->ncell   = tmp[0];
  grid->nnodes  = tmp[1];
  grid->nedge   = tmp[2];
  grid->nnbound = tmp[3];
  grid->nvbound = tmp[4];
  grid->nfbound = tmp[5];
  grid->nnfacet = tmp[6];
  grid->nvfacet = tmp[7];
  grid->nffacet = tmp[8];
  grid->nsnode = tmp[9];
  grid->nvnode = tmp[10];
  grid->nfnode = tmp[11];
  grid->ntte   = tmp[12];
  grid->nsface = 0;
  grid->nvface = 0;
  grid->nfface = 0;
  ierr = PetscFree(tmp);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"nnodes = %d,nedge = %d,nnfacet = %d,nsnode = %d,nfnode = %d\n",
              grid->nnodes,grid->nedge,grid->nnfacet,grid->nsnode,grid->nfnode);CHKERRQ(ierr);

  nnodes  = grid->nnodes;
  nedge   = grid->nedge;
  nnfacet = grid->nnfacet;
  nvfacet = grid->nvfacet;
  nffacet = grid->nffacet;
  nnbound = grid->nnbound;
  nvbound = grid->nvbound;
  nfbound = grid->nfbound;
  nsnode  = grid->nsnode;
  nvnode  = grid->nvnode;
  nfnode  = grid->nfnode;

  /* Read the partitioning vector generated by MeTiS */
  ICALLOC(nnodes,&l2a);
  ICALLOC(nnodes,&v2p);
  ICALLOC(nnodes,&a2l);
  nnodesLoc = 0;

  for (i = 0; i < nnodes; i++)  a2l[i] = -1;
  ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);

  if (!rank) {
    char spart_file[256],part_file[256];
    ierr = OptionsGetString(PETSC_NULL,"-partition",spart_file,256,&flg);CHKERRQ(ierr);
    sprintf(part_file,"%s.%d",spart_file,size);
    fptr = fopen(part_file,"r");
    if (!fptr) SETERRQ1(1,1,"Cannot open file %s\n",part_file);
    for (inode = 0; inode < nnodes; inode++) {
      fscanf(fptr,"%d\n",&node1); 
      v2p[inode] = node1;
    }
    fclose(fptr);
  }
  ierr = MPI_Bcast(v2p,nnodes,MPI_INT,0,comm);CHKERRQ(ierr);
  for (inode = 0; inode < nnodes; inode++) {
    if (v2p[inode] == rank) {
      l2a[nnodesLoc] = inode ; 
      a2l[inode]     = nnodesLoc ; 
      nnodesLoc++;
    } 
  }
  ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
  time_fin -= time_ini;
  ierr = PetscPrintf(comm,"Partition Vector read successfully\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Time taken in this phase was %g\n",time_fin);CHKERRQ(ierr);

  ierr = MPI_Scan(&nnodesLoc,&rstart,1,MPI_INT,MPI_SUM,comm);CHKERRQ(ierr);
  rstart -= nnodesLoc;
  ICALLOC(nnodesLoc,&pordering);
  for (i=0; i < nnodesLoc; i++) {
    pordering[i] = rstart + i;
  }
  ierr = AOCreateBasic(comm,nnodesLoc,l2a,pordering,&ao);CHKERRQ(ierr);
  ierr = PetscFree(pordering);CHKERRQ(ierr);

  /* Now count the local number of edges - including edges with 
   ghost nodes but edges between ghost nodes are NOT counted */
  nedgeLoc  = 0;
  nvertices = nnodesLoc;
  /* Choose an estimated number of local edges. The choice
   nedgeLocEst = 1000000 looks reasonable as it will read
   the edge and edge normal arrays in 8 MB chunks */ 
  /*nedgeLocEst = nedge/size;*/
  nedgeLocEst = PetscMin(nedge,1000000); 
  remEdges = nedge;
  ICALLOC(2*nedgeLocEst,&tmp);
  ierr = PetscSynchronizedBinarySeek(comm,fdes,0,BINARY_SEEK_CUR,&currentPos);CHKERRQ(ierr);
  ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);
  while (remEdges > 0) {
    readEdges = PetscMin(remEdges,nedgeLocEst); 
    /*time_ini = PetscGetTime();*/
    ierr = PetscSynchronizedBinaryRead(comm,fdes,tmp,readEdges,PETSC_INT);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinarySeek(comm,fdes,(nedge-readEdges)*BINARY_INT_SIZE,BINARY_SEEK_CUR,&newPos);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinaryRead(comm,fdes,tmp+readEdges,readEdges,PETSC_INT);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinarySeek(comm,fdes,-nedge*BINARY_INT_SIZE,BINARY_SEEK_CUR,&newPos);CHKERRQ(ierr);
    /*time_fin += PetscGetTime()-time_ini;*/
    for (j = 0; j < readEdges; j++) {
      node1 = tmp[j]-1;
      node2 = tmp[j+readEdges]-1;
      if ((v2p[node1] == rank) || (v2p[node2] == rank)) {
        nedgeLoc++;
        if (a2l[node1] == -1) {
          l2a[nvertices] = node1;
          a2l[node1] = nvertices;
          nvertices++;
        }
        if (a2l[node2] == -1) {
          l2a[nvertices] = node2;
          a2l[node2] = nvertices;
          nvertices++;
        }
      }
    }
    remEdges = remEdges - readEdges; 
    ierr = MPI_Barrier(comm);
  }
  ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
  time_fin -= time_ini;
  ierr = PetscPrintf(comm,"Local edges counted with MPI_Bcast %d\n",nedgeLoc);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local vertices counted %d\n",nvertices);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Time taken in this phase was %g\n",time_fin);CHKERRQ(ierr);

  /* Now store the local edges */
  ICALLOC(2*nedgeLoc,&grid->eptr);
  ICALLOC(nedgeLoc,&edge_bit);
  ICALLOC(nedgeLoc,&eperm);
  i = 0; j = 0; k = 0;
  remEdges = nedge;
  ierr = PetscSynchronizedBinarySeek(comm,fdes,currentPos,BINARY_SEEK_SET,&newPos);CHKERRQ(ierr);
  currentPos = newPos;

  ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);
  while (remEdges > 0) {
    readEdges = PetscMin(remEdges,nedgeLocEst); 
    ierr = PetscSynchronizedBinaryRead(comm,fdes,tmp,readEdges,PETSC_INT);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinarySeek(comm,fdes,(nedge-readEdges)*BINARY_INT_SIZE,BINARY_SEEK_CUR,&newPos);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinaryRead(comm,fdes,tmp+readEdges,readEdges,PETSC_INT);CHKERRQ(ierr);
    ierr = PetscSynchronizedBinarySeek(comm,fdes,-nedge*BINARY_INT_SIZE,BINARY_SEEK_CUR,&newPos);CHKERRQ(ierr);
    for (j = 0; j < readEdges; j++) {
      node1 = tmp[j]-1;
      node2 = tmp[j+readEdges]-1;
      if ((v2p[node1] == rank) || (v2p[node2] == rank)) {
        grid->eptr[k] = a2l[node1];
        grid->eptr[k+nedgeLoc] = a2l[node2];
        edge_bit[k] = i;
        eperm[k] = k;
        k++;
      }
      i++;
    }
    remEdges = remEdges - readEdges; 
    ierr = MPI_Barrier(comm);
  }
  ierr = PetscSynchronizedBinarySeek(comm,fdes,currentPos+2*nedge*BINARY_INT_SIZE,BINARY_SEEK_SET,&newPos);CHKERRQ(ierr);
  ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
  time_fin -= time_ini;
  ierr = PetscPrintf(comm,"Local edges stored\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Time taken in this phase was %g\n",time_fin);CHKERRQ(ierr);

  ierr = PetscFree(tmp);CHKERRQ(ierr);
  ICALLOC(2*nedgeLoc,&tmp);
  /* Now reorder the edges for better cache locality */     
  /*
  tmp[0]=7;tmp[1]=6;tmp[2]=3;tmp[3]=9;tmp[4]=2;tmp[5]=0;
  ierr = PetscSortIntWithPermutation(6,tmp,eperm); 
  for (i=0; i<6; i++)
   printf("%d %d %d\n",i,tmp[i],eperm[i]);
  */
  ierr = PetscMemcpy(tmp,grid->eptr,2*nedgeLoc*sizeof(int));CHKERRQ(ierr);
  ierr = OptionsHasName(0,"-no_edge_reordering",&flg);CHKERRQ(ierr);
  if (!flg) {
   ierr = PetscSortIntWithPermutation(nedgeLoc,tmp,eperm);CHKERRQ(ierr);
  }
  k = 0;
  for (i = 0; i < nedgeLoc; i++) {
#if defined(INTERLACING) 
    grid->eptr[k++] = tmp[eperm[i]] + 1;
    grid->eptr[k++] = tmp[nedgeLoc+eperm[i]] + 1;
#else
    grid->eptr[i] = tmp[eperm[i]] + 1;
    grid->eptr[nedgeLoc+i] = tmp[nedgeLoc+eperm[i]] + 1;
#endif
  }
  ierr = PetscFree(tmp);CHKERRQ(ierr);

  /* Now make the local 'ia' and 'ja' arrays */
  ICALLOC(nnodesLoc+1,&grid->ia);
  /* Use tmp for a work array */
  ICALLOC(nnodesLoc,&tmp);
  f77GETIA(&nnodesLoc,&nedgeLoc,grid->eptr,grid->ia,tmp,&rank);
  nnz = grid->ia[nnodesLoc] - 1;
  ICALLOC(nnz,&grid->ja);
  f77GETJA(&nnodesLoc,&nedgeLoc,grid->eptr,grid->ia,grid->ja,tmp,&rank);

  ierr = PetscFree(tmp);CHKERRQ(ierr);

  ICALLOC(nvertices,&grid->loc2glo);
  ierr = PetscMemcpy(grid->loc2glo,l2a,nvertices*sizeof(int));CHKERRQ(ierr);
  ierr = PetscFree(l2a);CHKERRQ(ierr);
  l2a = grid->loc2glo;
  ICALLOC(nvertices,&grid->loc2pet);
  l2p = grid->loc2pet;
  ierr = PetscMemcpy(l2p,l2a,nvertices*sizeof(int));CHKERRQ(ierr);
  ierr = AOApplicationToPetsc(ao,nvertices,l2p);CHKERRQ(ierr);

/* Map the 'ja' array in petsc ordering */
  nnz = grid->ia[nnodesLoc] - 1;
  for (i = 0; i < nnz; i++){
    grid->ja[i] = l2a[grid->ja[i] - 1];
  }
  ierr = AOApplicationToPetsc(ao,nnz,grid->ja);CHKERRQ(ierr);
  ierr = AODestroy(ao);CHKERRQ(ierr);

 /* Renumber unit normals of dual face (from node1 to node2)
     and the area of the dual mesh face */
  FCALLOC(nedgeLocEst,&ftmp);
  FCALLOC(nedgeLoc,&ftmp1);
  FCALLOC(4*nedgeLoc,&grid->xyzn);
  /* Do the x-component */
  i = 0; k = 0;
  remEdges = nedge;
  ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);
  while (remEdges > 0) {
   readEdges = PetscMin(remEdges,nedgeLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readEdges,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readEdges; j++) {
     if (edge_bit[k] == (i+j)) {
      ftmp1[k] = ftmp[j];
      k++;
     }
   }
   i+= readEdges;
   remEdges = remEdges - readEdges; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }
  for (i = 0; i < nedgeLoc; i++) 
#if defined(INTERLACING) 
   grid->xyzn[4*i] = ftmp1[eperm[i]];
#else
   grid->xyzn[i] = ftmp1[eperm[i]];
#endif
  /* Do the y-component */
  i = 0; k = 0;
  remEdges = nedge;
  while (remEdges > 0) {
   readEdges = PetscMin(remEdges,nedgeLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readEdges,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readEdges; j++) {
     if (edge_bit[k] == (i+j)) {
      ftmp1[k] = ftmp[j];
      k++;
     }
   }
   i+= readEdges;
   remEdges = remEdges - readEdges; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }
  for (i = 0; i < nedgeLoc; i++) 
#if defined(INTERLACING) 
   grid->xyzn[4*i+1] = ftmp1[eperm[i]];
#else
   grid->xyzn[nedgeLoc+i] = ftmp1[eperm[i]];
#endif
  /* Do the z-component */
  i = 0; k = 0;
  remEdges = nedge;
  while (remEdges > 0) {
   readEdges = PetscMin(remEdges,nedgeLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readEdges,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readEdges; j++) {
     if (edge_bit[k] == (i+j)) {
      ftmp1[k] = ftmp[j];
      k++;
     }
   }
   i+= readEdges;
   remEdges = remEdges - readEdges; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }
  for (i = 0; i < nedgeLoc; i++) 
#if defined(INTERLACING) 
   grid->xyzn[4*i+2] = ftmp1[eperm[i]];
#else
   grid->xyzn[2*nedgeLoc+i] = ftmp1[eperm[i]];
#endif
  /* Do the length */
  i = 0; k = 0;
  remEdges = nedge;
  while (remEdges > 0) {
   readEdges = PetscMin(remEdges,nedgeLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readEdges,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readEdges; j++) {
     if (edge_bit[k] == (i+j)) {
      ftmp1[k] = ftmp[j];
      k++;
     }
   }
   i+= readEdges;
   remEdges = remEdges - readEdges; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }
  for (i = 0; i < nedgeLoc; i++) 
#if defined(INTERLACING) 
   grid->xyzn[4*i+3] = ftmp1[eperm[i]];
#else
   grid->xyzn[3*nedgeLoc+i] = ftmp1[eperm[i]];
#endif

  ierr = PetscFree(edge_bit);CHKERRQ(ierr);
  ierr = PetscFree(eperm);CHKERRQ(ierr);
  ierr = PetscFree(ftmp);CHKERRQ(ierr);
  ierr = PetscFree(ftmp1);CHKERRQ(ierr);
  ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
  time_fin -= time_ini;
  ierr = PetscPrintf(comm,"Edge normals partitioned\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Time taken in this phase was %g\n",time_fin);CHKERRQ(ierr);

  /* Remap coordinates */
  /*nnodesLocEst = nnodes/size;*/
  nnodesLocEst = PetscMin(nnodes,500000);
  FCALLOC(nnodesLocEst,&ftmp);
  FCALLOC(3*nvertices,&grid->xyz);
  remNodes = nnodes;
  i = 0;
  ierr = PetscGetTime(&time_ini);CHKERRQ(ierr);
  while (remNodes > 0) {
   readNodes = PetscMin(remNodes,nnodesLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readNodes,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readNodes; j++) {
     if (a2l[i+j] >= 0) {
#if defined(INTERLACING) 
      grid->xyz[3*a2l[i+j]] = ftmp[j];
#else
      grid->xyz[a2l[i+j]] = ftmp[j];
#endif
     }
   }
   i+= nnodesLocEst;
   remNodes -= nnodesLocEst; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }

  remNodes = nnodes;
  i = 0;
  while (remNodes > 0) {
   readNodes = PetscMin(remNodes,nnodesLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readNodes,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readNodes; j++) {
     if (a2l[i+j] >= 0) {
#if defined(INTERLACING) 
      grid->xyz[3*a2l[i+j]+1] = ftmp[j];
#else
      grid->xyz[nnodesLoc+a2l[i+j]] = ftmp[j];
#endif
     }
   }
   i+= nnodesLocEst;
   remNodes -= nnodesLocEst; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }

  remNodes = nnodes;
  i = 0;
  while (remNodes > 0) {
   readNodes = PetscMin(remNodes,nnodesLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readNodes,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readNodes; j++) {
     if (a2l[i+j] >= 0) {
#if defined(INTERLACING) 
      grid->xyz[3*a2l[i+j]+2] = ftmp[j];
#else
      grid->xyz[2*nnodesLoc+a2l[i+j]] = ftmp[j];
#endif
     }
   }
   i+= nnodesLocEst;
   remNodes -= nnodesLocEst; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }


  /* Renumber dual volume */
  FCALLOC(nvertices,&grid->area);
  remNodes = nnodes;
  i = 0;
  while (remNodes > 0) {
   readNodes = PetscMin(remNodes,nnodesLocEst); 
   ierr = PetscSynchronizedBinaryRead(comm,fdes,ftmp,readNodes,PETSC_SCALAR);CHKERRQ(ierr);
   for (j = 0; j < readNodes; j++) {
     if (a2l[i+j] >= 0) {
      grid->area[a2l[i+j]] = ftmp[j];
     }
   }
   i+= nnodesLocEst;
   remNodes -= nnodesLocEst; 
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);
  }

  ierr = PetscFree(ftmp);CHKERRQ(ierr);
  ierr = PetscGetTime(&time_fin);CHKERRQ(ierr);
  time_fin -= time_ini;
  ierr = PetscPrintf(comm,"Coordinates remapped\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Time taken in this phase was %g\n",time_fin);CHKERRQ(ierr);

/* Now,handle all the solid boundaries - things to be done :
 * 1. Identify the nodes belonging to the solid  
 *    boundaries and count them.
 * 2. Put proper indices into f2ntn array,after making it
 *    of suitable size.
 * 3. Remap the normals and areas of solid faces (sxn,syn,szn,
 *    and sa arrays). 
 */
  ICALLOC(nnbound,  &grid->nntet);
  ICALLOC(nnbound,  &grid->nnpts);
  ICALLOC(4*nnfacet,&grid->f2ntn);
  ICALLOC(nsnode,&grid->isnode);
  FCALLOC(nsnode,&grid->sxn);
  FCALLOC(nsnode,&grid->syn);
  FCALLOC(nsnode,&grid->szn);
  FCALLOC(nsnode,&grid->sa);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nntet,nnbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nnpts,nnbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->f2ntn,4*nnfacet,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->isnode,nsnode,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->sxn,nsnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->syn,nsnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->szn,nsnode,PETSC_SCALAR);CHKERRQ(ierr);

  isurf = 0;
  nsnodeLoc = 0;
  nnfacetLoc = 0;
  nb = 0;
  nte = 0;
  ICALLOC(3*nnfacet,&tmp);
  ICALLOC(nsnode,&tmp1);
  ICALLOC(nnodes,&tmp2);
  FCALLOC(4*nsnode,&ftmp);
  ierr = PetscMemzero(tmp,3*nnfacet*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp1,nsnode*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp2,nnodes*sizeof(int));CHKERRQ(ierr);

  j = 0;
  for (i = 0; i < nsnode; i++) {
    node1 = a2l[grid->isnode[i] - 1];
    if (node1 >= 0) {
     tmp1[nsnodeLoc] = node1;
     tmp2[node1] = nsnodeLoc;
     ftmp[j++] = grid->sxn[i];
     ftmp[j++] = grid->syn[i];
     ftmp[j++] = grid->szn[i];
     ftmp[j++] = grid->sa[i];
     nsnodeLoc++;
    }
  }
  for (i = 0; i < nnbound; i++) {
   for (j = isurf; j < isurf + grid->nntet[i]; j++) {
    node1 = a2l[grid->isnode[grid->f2ntn[j] - 1] - 1];
    node2 = a2l[grid->isnode[grid->f2ntn[nnfacet + j] - 1] - 1];
    node3 = a2l[grid->isnode[grid->f2ntn[2*nnfacet + j] - 1] - 1];
    
    if ((node1 >= 0) && (node2 >= 0) && (node3 >= 0)) {
       nnfacetLoc++;
       nte++;
       tmp[nb++] = tmp2[node1];
       tmp[nb++] = tmp2[node2];
       tmp[nb++] = tmp2[node3];
    }
   }
   isurf += grid->nntet[i];
   /*printf("grid->nntet[%d] before reordering is %d\n",i,grid->nntet[i]);*/
   grid->nntet[i] = nte;
   /*printf("grid->nntet[%d] after reordering is %d\n",i,grid->nntet[i]);*/
   nte = 0;
  }
  ierr = PetscFree(grid->f2ntn);CHKERRQ(ierr);
  ierr = PetscFree(grid->isnode);CHKERRQ(ierr);
  ierr = PetscFree(grid->sxn);CHKERRQ(ierr);
  ierr = PetscFree(grid->syn);CHKERRQ(ierr);
  ierr = PetscFree(grid->szn);CHKERRQ(ierr);
  ierr = PetscFree(grid->sa);CHKERRQ(ierr);
  ICALLOC(4*nnfacetLoc,&grid->f2ntn);
  ICALLOC(nsnodeLoc,&grid->isnode);
  FCALLOC(nsnodeLoc,&grid->sxn);
  FCALLOC(nsnodeLoc,&grid->syn);
  FCALLOC(nsnodeLoc,&grid->szn);
  FCALLOC(nsnodeLoc,&grid->sa);
  j = 0;
  for (i = 0; i < nsnodeLoc; i++) {
   grid->isnode[i] = tmp1[i] + 1;
   grid->sxn[i] = ftmp[j++];
   grid->syn[i] = ftmp[j++];
   grid->szn[i] = ftmp[j++];
   grid->sa[i] = ftmp[j++];
  }
  j = 0;
  for (i = 0; i < nnfacetLoc; i++) {
   grid->f2ntn[i] = tmp[j++] + 1; 
   grid->f2ntn[nnfacetLoc+i] = tmp[j++] + 1; 
   grid->f2ntn[2*nnfacetLoc+i] = tmp[j++] + 1; 
  }
 ierr = PetscFree(tmp);CHKERRQ(ierr);
 ierr = PetscFree(tmp1);CHKERRQ(ierr);
 ierr = PetscFree(tmp2);CHKERRQ(ierr);
 ierr = PetscFree(ftmp);CHKERRQ(ierr);

/* Now identify the triangles on which the current proceesor
   would perform force calculation */
  ICALLOC(nnfacetLoc,&grid->sface_bit);
  PetscMemzero(grid->sface_bit,nnfacetLoc*sizeof(int));
  for (i = 0; i < nnfacetLoc; i++) {
    node1 = l2a[grid->isnode[grid->f2ntn[i] - 1] - 1];
    node2 = l2a[grid->isnode[grid->f2ntn[nnfacetLoc + i] - 1] - 1];
    node3 = l2a[grid->isnode[grid->f2ntn[2*nnfacetLoc + i] - 1] - 1];
    if (((v2p[node1] >= rank) && (v2p[node2] >= rank) 
        && (v2p[node3] >= rank)) &&
        ((v2p[node1] == rank) || (v2p[node2] == rank)
        || (v2p[node3] == rank))) {
         grid->sface_bit[i] = 1;
    }
  }
  /*printf("On processor %d total solid triangles = %d,locally owned = %d alpha = %d\n",rank,totTr,myTr,alpha);*/
  ierr = PetscPrintf(comm,"Solid boundaries partitioned\n");CHKERRQ(ierr);

/* Now,handle all the viscous boundaries - things to be done :
 * 1. Identify the nodes belonging to the viscous
 *    boundaries and count them.
 * 2. Put proper indices into f2ntv array,after making it
 *    of suitable size
 * 3. Remap the normals and areas of viscous faces (vxn,vyn,vzn,
 *    and va arrays). 
 */
  ICALLOC(nvbound,  &grid->nvtet);
  ICALLOC(nvbound,  &grid->nvpts);
  ICALLOC(4*nvfacet,&grid->f2ntv);
  ICALLOC(nvnode,&grid->ivnode);
  FCALLOC(nvnode,&grid->vxn);
  FCALLOC(nvnode,&grid->vyn);
  FCALLOC(nvnode,&grid->vzn);
  FCALLOC(nvnode,&grid->va);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nvtet,nvbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nvpts,nvbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->f2ntv,4*nvfacet,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->ivnode,nvnode,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->vxn,nvnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->vyn,nvnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->vzn,nvnode,PETSC_SCALAR);CHKERRQ(ierr);

  isurf = 0;
  nvnodeLoc = 0;
  nvfacetLoc = 0;
  nb = 0;
  nte = 0;
  ICALLOC(3*nvfacet,&tmp);
  ICALLOC(nvnode,&tmp1);
  ICALLOC(nnodes,&tmp2);
  FCALLOC(4*nvnode,&ftmp);
  ierr = PetscMemzero(tmp,3*nvfacet*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp1,nvnode*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp2,nnodes*sizeof(int));CHKERRQ(ierr);

  j = 0;
  for (i = 0; i < nvnode; i++) {
    node1 = a2l[grid->ivnode[i] - 1];
    if (node1 >= 0) {
     tmp1[nvnodeLoc] = node1;
     tmp2[node1] = nvnodeLoc;
     ftmp[j++] = grid->vxn[i];
     ftmp[j++] = grid->vyn[i];
     ftmp[j++] = grid->vzn[i];
     ftmp[j++] = grid->va[i];
     nvnodeLoc++;
    }
  }
  for (i = 0; i < nvbound; i++) {
   for (j = isurf; j < isurf + grid->nvtet[i]; j++) {
    node1 = a2l[grid->ivnode[grid->f2ntv[j] - 1] - 1];
    node2 = a2l[grid->ivnode[grid->f2ntv[nvfacet + j] - 1] - 1];
    node3 = a2l[grid->ivnode[grid->f2ntv[2*nvfacet + j] - 1] - 1];
    if ((node1 >= 0) && (node2 >= 0) && (node3 >= 0)) {
       nvfacetLoc++;
       nte++;
       tmp[nb++] = tmp2[node1];
       tmp[nb++] = tmp2[node2];
       tmp[nb++] = tmp2[node3];
    }
   }
   isurf += grid->nvtet[i];
   grid->nvtet[i] = nte;
   nte = 0;
  }
  ierr = PetscFree(grid->f2ntv);CHKERRQ(ierr);
  ierr = PetscFree(grid->ivnode);CHKERRQ(ierr);
  ierr = PetscFree(grid->vxn);CHKERRQ(ierr);
  ierr = PetscFree(grid->vyn);CHKERRQ(ierr);
  ierr = PetscFree(grid->vzn);CHKERRQ(ierr);
  ierr = PetscFree(grid->va);CHKERRQ(ierr);
  ICALLOC(4*nvfacetLoc,&grid->f2ntv);
  ICALLOC(nvnodeLoc,&grid->ivnode);
  FCALLOC(nvnodeLoc,&grid->vxn);
  FCALLOC(nvnodeLoc,&grid->vyn);
  FCALLOC(nvnodeLoc,&grid->vzn);
  FCALLOC(nvnodeLoc,&grid->va);
  j = 0;
  for (i = 0; i < nvnodeLoc; i++) {
   grid->ivnode[i] = tmp1[i] + 1;
   grid->vxn[i] = ftmp[j++];
   grid->vyn[i] = ftmp[j++];
   grid->vzn[i] = ftmp[j++];
   grid->va[i] = ftmp[j++];
  }
  j = 0;
  for (i = 0; i < nvfacetLoc; i++) {
   grid->f2ntv[i] = tmp[j++] + 1;
   grid->f2ntv[nvfacetLoc+i] = tmp[j++] + 1;
   grid->f2ntv[2*nvfacetLoc+i] = tmp[j++] + 1;
  }
 ierr = PetscFree(tmp);CHKERRQ(ierr);
 ierr = PetscFree(tmp1);CHKERRQ(ierr);
 ierr = PetscFree(tmp2);CHKERRQ(ierr);
 ierr = PetscFree(ftmp);CHKERRQ(ierr);

/* Now identify the triangles on which the current proceesor
   would perform force calculation */
  ICALLOC(nvfacetLoc,&grid->vface_bit);
  ierr = PetscMemzero(grid->vface_bit,nvfacetLoc*sizeof(int));CHKERRQ(ierr);
  for (i = 0; i < nvfacetLoc; i++) {
    node1 = l2a[grid->ivnode[grid->f2ntv[i] - 1] - 1];
    node2 = l2a[grid->ivnode[grid->f2ntv[nvfacetLoc + i] - 1] - 1];
    node3 = l2a[grid->ivnode[grid->f2ntv[2*nvfacetLoc + i] - 1] - 1];
    if (((v2p[node1] >= rank) && (v2p[node2] >= rank)
        && (v2p[node3] >= rank)) &&
        ((v2p[node1] == rank) || (v2p[node2] == rank)
        || (v2p[node3] == rank))) {
         grid->vface_bit[i] = 1;
    }
  }
  ierr = PetscFree(v2p);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Viscous boundaries partitioned\n");CHKERRQ(ierr);
 
/* Now,handle all the free boundaries - things to be done :
 * 1. Identify the nodes belonging to the free
 *    boundaries and count them.
 * 2. Put proper indices into f2ntf array,after making it
 *    of suitable size
 * 3. Remap the normals and areas of free bound. faces (fxn,fyn,fzn,
 *    and fa arrays). 
 */
 
  ICALLOC(nfbound,  &grid->nftet);
  ICALLOC(nfbound,  &grid->nfpts);
  ICALLOC(4*nffacet,&grid->f2ntf);
  ICALLOC(nfnode,&grid->ifnode);
  FCALLOC(nfnode,&grid->fxn);
  FCALLOC(nfnode,&grid->fyn);
  FCALLOC(nfnode,&grid->fzn);
  FCALLOC(nfnode,&grid->fa);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nftet,nfbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->nfpts,nfbound,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->f2ntf,4*nffacet,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->ifnode,nfnode,PETSC_INT);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->fxn,nfnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->fyn,nfnode,PETSC_SCALAR);CHKERRQ(ierr);
  ierr = PetscSynchronizedBinaryRead(comm,fdes,grid->fzn,nfnode,PETSC_SCALAR);CHKERRQ(ierr);

  isurf = 0;
  nfnodeLoc = 0;
  nffacetLoc = 0;
  nb = 0;
  nte = 0;
  ICALLOC(3*nffacet,&tmp);
  ICALLOC(nfnode,&tmp1);
  ICALLOC(nnodes,&tmp2);
  FCALLOC(4*nfnode,&ftmp);
  ierr = PetscMemzero(tmp,3*nffacet*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp1,nfnode*sizeof(int));CHKERRQ(ierr);
  ierr = PetscMemzero(tmp2,nnodes*sizeof(int));CHKERRQ(ierr);

  j = 0;
  for (i = 0; i < nfnode; i++) {
    node1 = a2l[grid->ifnode[i] - 1];
    if (node1 >= 0) {
     tmp1[nfnodeLoc] = node1;
     tmp2[node1] = nfnodeLoc;
     ftmp[j++] = grid->fxn[i];
     ftmp[j++] = grid->fyn[i];
     ftmp[j++] = grid->fzn[i];
     ftmp[j++] = grid->fa[i];
     nfnodeLoc++;
    }
  }
  for (i = 0; i < nfbound; i++) {
   for (j = isurf; j < isurf + grid->nftet[i]; j++) {
    node1 = a2l[grid->ifnode[grid->f2ntf[j] - 1] - 1];
    node2 = a2l[grid->ifnode[grid->f2ntf[nffacet + j] - 1] - 1];
    node3 = a2l[grid->ifnode[grid->f2ntf[2*nffacet + j] - 1] - 1];
    if ((node1 >= 0) && (node2 >= 0) && (node3 >= 0)) {
       nffacetLoc++;
       nte++;
       tmp[nb++] = tmp2[node1];
       tmp[nb++] = tmp2[node2];
       tmp[nb++] = tmp2[node3];
    }
   }
   isurf += grid->nftet[i];
   grid->nftet[i] = nte;
   nte = 0;
  }
  ierr = PetscFree(grid->f2ntf);CHKERRQ(ierr);
  ierr = PetscFree(grid->ifnode);CHKERRQ(ierr);
  ierr = PetscFree(grid->fxn);CHKERRQ(ierr);
  ierr = PetscFree(grid->fyn);CHKERRQ(ierr);
  ierr = PetscFree(grid->fzn);CHKERRQ(ierr);
  ierr = PetscFree(grid->fa);CHKERRQ(ierr);
  ICALLOC(4*nffacetLoc,&grid->f2ntf);
  ICALLOC(nfnodeLoc,&grid->ifnode);
  FCALLOC(nfnodeLoc,&grid->fxn);
  FCALLOC(nfnodeLoc,&grid->fyn);
  FCALLOC(nfnodeLoc,&grid->fzn);
  FCALLOC(nfnodeLoc,&grid->fa);
  j = 0;
  for (i = 0; i < nfnodeLoc; i++) {
   grid->ifnode[i] = tmp1[i] + 1;
   grid->fxn[i] = ftmp[j++];
   grid->fyn[i] = ftmp[j++];
   grid->fzn[i] = ftmp[j++];
   grid->fa[i] = ftmp[j++];
  }
  j = 0;
  for (i = 0; i < nffacetLoc; i++) {
   grid->f2ntf[i] = tmp[j++] + 1;
   grid->f2ntf[nffacetLoc+i] = tmp[j++] + 1;
   grid->f2ntf[2*nffacetLoc+i] = tmp[j++] + 1;
  }

 
  ierr = PetscFree(tmp);CHKERRQ(ierr);
  ierr = PetscFree(tmp1);CHKERRQ(ierr);
  ierr = PetscFree(tmp2);CHKERRQ(ierr);
  ierr = PetscFree(ftmp);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Free boundaries partitioned\n");CHKERRQ(ierr);

  ierr = OptionsHasName(0,"-mem_use",&flg);CHKERRQ(ierr);
  if (flg) {
    ierr = PetscShowMemoryUsage(VIEWER_STDOUT_WORLD,"Memory usage after partitioning\n");CHKERRQ(ierr);
  }

 /* Put different mappings and other info into grid */
  /* ICALLOC(nvertices,&grid->loc2pet); 
   ICALLOC(nvertices,&grid->loc2glo);
   PetscMemcpy(grid->loc2pet,l2p,nvertices*sizeof(int));
   PetscMemcpy(grid->loc2glo,l2a,nvertices*sizeof(int));
   ierr = PetscFree(l2a);CHKERRQ(ierr);
   ierr = PetscFree(l2p);CHKERRQ(ierr);*/
  
   grid->nnodesLoc = nnodesLoc;
   grid->nedgeLoc = nedgeLoc;
   grid->nvertices = nvertices;
   grid->nsnodeLoc = nsnodeLoc;
   grid->nvnodeLoc = nvnodeLoc;
   grid->nfnodeLoc = nfnodeLoc;
   grid->nnfacetLoc = nnfacetLoc;
   grid->nvfacetLoc = nvfacetLoc;
   grid->nffacetLoc = nffacetLoc;
/*
 * FCALLOC(nvertices*4, &grid->gradx);
 * FCALLOC(nvertices*4, &grid->grady);
 * FCALLOC(nvertices*4, &grid->gradz);
 */
   FCALLOC(nvertices,   &grid->cdt);
   FCALLOC(nvertices*4, &grid->phi);
/*
   FCALLOC(nvertices,   &grid->r11);
   FCALLOC(nvertices,   &grid->r12);
   FCALLOC(nvertices,   &grid->r13);
   FCALLOC(nvertices,   &grid->r22);
   FCALLOC(nvertices,   &grid->r23);
   FCALLOC(nvertices,   &grid->r33);
*/
   FCALLOC(7*nnodesLoc,   &grid->rxy);
   FCALLOC(4*nnodesLoc,   &grid->resd);

/* Print the different mappings
 *
 */
 {
  int partLoc[7],partMax[7],partMin[7],partSum[7];
  partLoc[0] = nnodesLoc;
  partLoc[1] = nvertices;
  partLoc[2] = nedgeLoc;
  partLoc[3] = nnfacetLoc;
  partLoc[4] = nffacetLoc;
  partLoc[5] = nsnodeLoc;
  partLoc[6] = nfnodeLoc;
  for (i = 0; i < 7; i++) {
   partMin[i] = 0;
   partMax[i] = 0;
   partSum[i] = 0;
  }

  ierr = MPI_Allreduce(partLoc,partMax,7,MPI_INT,MPI_MAX,comm);CHKERRQ(ierr);
  ierr = MPI_Allreduce(partLoc,partMin,7,MPI_INT,MPI_MIN,comm);CHKERRQ(ierr);
  ierr = MPI_Allreduce(partLoc,partSum,7,MPI_INT,MPI_SUM,comm);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"==============================\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Partitioning quality info ....\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"==============================\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"------------------------------------------------------------\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Item                    Min        Max    Average      Total\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"------------------------------------------------------------\n");CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local Nodes       %9d  %9d  %9d  %9d\n",
              partMin[0],partMax[0],partSum[0]/size,partSum[0]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local+Ghost Nodes %9d  %9d  %9d  %9d\n",
              partMin[1],partMax[1],partSum[1]/size,partSum[1]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local Edges       %9d  %9d  %9d  %9d\n",
              partMin[2],partMax[2],partSum[2]/size,partSum[2]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local solid faces %9d  %9d  %9d  %9d\n",
              partMin[3],partMax[3],partSum[3]/size,partSum[3]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local free faces  %9d  %9d  %9d  %9d\n",
              partMin[4],partMax[4],partSum[4]/size,partSum[4]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local solid nodes %9d  %9d  %9d  %9d\n",
              partMin[5],partMax[5],partSum[5]/size,partSum[5]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"Local free nodes  %9d  %9d  %9d  %9d\n",
              partMin[6],partMax[6],partSum[6]/size,partSum[6]);CHKERRQ(ierr);
  ierr = PetscPrintf(comm,"------------------------------------------------------------\n");CHKERRQ(ierr);
 }
 ierr = OptionsHasName(0,"-partition_info",&flg);CHKERRQ(ierr);
 if (flg) {
  char part_file[256];
  sprintf(part_file,"output.%d",rank);
  fptr1 = fopen(part_file,"w");

  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local and Global Grid Parameters are :\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local\t\t\t\tGlobal\n");
  fprintf(fptr1,"nnodesLoc = %d\t\tnnodes = %d\n",nnodesLoc,nnodes);
  fprintf(fptr1,"nedgeLoc = %d\t\t\tnedge = %d\n",nedgeLoc,nedge);
  fprintf(fptr1,"nnfacetLoc = %d\t\tnnfacet = %d\n",nnfacetLoc,nnfacet);
  fprintf(fptr1,"nvfacetLoc = %d\t\t\tnvfacet = %d\n",nvfacetLoc,nvfacet);
  fprintf(fptr1,"nffacetLoc = %d\t\t\tnffacet = %d\n",nffacetLoc,nffacet);
  fprintf(fptr1,"nsnodeLoc = %d\t\t\tnsnode = %d\n",nsnodeLoc,nsnode);
  fprintf(fptr1,"nvnodeLoc = %d\t\t\tnvnode = %d\n",nvnodeLoc,nvnode);
  fprintf(fptr1,"nfnodeLoc = %d\t\t\tnfnode = %d\n",nfnodeLoc,nfnode);
  fprintf(fptr1,"\n");
  fprintf(fptr1,"nvertices = %d\n",nvertices);
  fprintf(fptr1,"nnbound = %d\n",nnbound);
  fprintf(fptr1,"nvbound = %d\n",nvbound);
  fprintf(fptr1,"nfbound = %d\n",nfbound);
  fprintf(fptr1,"\n");

  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Different Orderings\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local\t\tPETSc\t\tGlobal\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nvertices; i++) {
   fprintf(fptr1,"%d\t\t%d\t\t%d\n",i,grid->loc2pet[i],grid->loc2glo[i]);
  }
  fprintf(fptr1,"\n");

  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Solid Boundary Nodes\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local\t\tPETSc\t\tGlobal\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nsnodeLoc; i++) {
   j = grid->isnode[i]-1;
   fprintf(fptr1,"%d\t\t%d\t\t%d\n",j,grid->loc2pet[j],grid->loc2glo[j]);
  }
  fprintf(fptr1,"\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"f2ntn array\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nnfacetLoc; i++) {
   fprintf(fptr1,"%d\t\t%d\t\t%d\t\t%d\n",i,grid->f2ntn[i],
           grid->f2ntn[nnfacetLoc+i],grid->f2ntn[2*nnfacetLoc+i]);
  }
  fprintf(fptr1,"\n");


  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Viscous Boundary Nodes\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local\t\tPETSc\t\tGlobal\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nvnodeLoc; i++) {
   j = grid->ivnode[i]-1;
   fprintf(fptr1,"%d\t\t%d\t\t%d\n",j,grid->loc2pet[j],grid->loc2glo[j]);
  }
  fprintf(fptr1,"\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"f2ntv array\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nvfacetLoc; i++) {
   fprintf(fptr1,"%d\t\t%d\t\t%d\t\t%d\n",i,grid->f2ntv[i],
           grid->f2ntv[nvfacetLoc+i],grid->f2ntv[2*nvfacetLoc+i]);
  }
  fprintf(fptr1,"\n");

  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Free Boundary Nodes\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Local\t\tPETSc\t\tGlobal\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nfnodeLoc; i++) {
   j = grid->ifnode[i]-1;
   fprintf(fptr1,"%d\t\t%d\t\t%d\n",j,grid->loc2pet[j],grid->loc2glo[j]);
  }
  fprintf(fptr1,"\n");
  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"f2ntf array\n");
  fprintf(fptr1,"---------------------------------------------\n");
  for (i = 0; i < nffacetLoc; i++) {
   fprintf(fptr1,"%d\t\t%d\t\t%d\t\t%d\n",i,grid->f2ntf[i],
           grid->f2ntf[nffacetLoc+i],grid->f2ntf[2*nffacetLoc+i]);
  }
  fprintf(fptr1,"\n");

  fprintf(fptr1,"---------------------------------------------\n");
  fprintf(fptr1,"Neighborhood Info In Various Ordering\n");
  fprintf(fptr1,"---------------------------------------------\n");
  ICALLOC(nnodes,&p2l);
  for (i = 0; i < nvertices; i++)
   p2l[grid->loc2pet[i]] = i;
  for (i = 0; i < nnodesLoc; i++) {
   jstart = grid->ia[grid->loc2glo[i]] - 1;
   jend = grid->ia[grid->loc2glo[i]+1] - 1;
   fprintf(fptr1,"Neighbors of Node %d in Local Ordering are :",i);
   for (j = jstart; j < jend; j++) {
    fprintf(fptr1,"%d ",p2l[grid->ja[j]]);
   }
   fprintf(fptr1,"\n");

   fprintf(fptr1,"Neighbors of Node %d in PETSc ordering are :",grid->loc2pet[i]);
   for (j = jstart; j < jend; j++) {
    fprintf(fptr1,"%d ",grid->ja[j]);
   }
   fprintf(fptr1,"\n");

   fprintf(fptr1,"Neighbors of Node %d in Global Ordering are :",grid->loc2glo[i]);
   for (j = jstart; j < jend; j++) {
    fprintf(fptr1,"%d ",grid->loc2glo[p2l[grid->ja[j]]]);
   }
   fprintf(fptr1,"\n");
 
  }
  fprintf(fptr1,"\n");
  ierr = PetscFree(p2l);CHKERRQ(ierr);
  fclose(fptr1);
 }

/* Free the temporary arrays */
   ierr = PetscFree(a2l);CHKERRQ(ierr);
   ierr = MPI_Barrier(comm);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}


/*---------------------------------------------------------------------*/
#undef __FUNC__
#define __FUNC__ "SetPetscDS"
int SetPetscDS(GRID *grid,TstepCtx *tsCtx)
/*---------------------------------------------------------------------*/
{
   int                    ierr,i,j,k,bs;
   int                    nnodes,jstart,jend,nbrs_diag,nbrs_offd;
   int                    nnodesLoc,nvertices;
   int                    *val_diag,*val_offd,*svertices,*loc2pet;
   IS                     isglobal,islocal;
   ISLocalToGlobalMapping isl2g;
   PetscTruth             flg;
   MPI_Comm               comm = PETSC_COMM_WORLD;

   PetscFunctionBegin;
   nnodes    = grid->nnodes;
   nnodesLoc = grid->nnodesLoc;
   nvertices = grid->nvertices;
   loc2pet   = grid->loc2pet;
   bs        = 4;

   /* Set up the PETSc datastructures */
 
   ierr = VecCreateMPI(comm,bs*nnodesLoc,bs*nnodes,&grid->qnode);CHKERRQ(ierr);
   ierr = VecDuplicate(grid->qnode,&grid->res);CHKERRQ(ierr);
   ierr = VecDuplicate(grid->qnode,&tsCtx->qold);CHKERRQ(ierr);
   ierr = VecDuplicate(grid->qnode,&tsCtx->func);CHKERRQ(ierr);
   ierr = VecCreateSeq(MPI_COMM_SELF,bs*nvertices,&grid->qnodeLoc);CHKERRQ(ierr);
   ierr = VecCreateMPI(comm,3*bs*nnodesLoc,3*bs*nnodes,&grid->grad);
   ierr = VecCreateSeq(MPI_COMM_SELF,3*bs*nvertices,&grid->gradLoc);
/* Create Scatter between the local and global vectors */
/* First create scatter for qnode */
   ierr = ISCreateStride(MPI_COMM_SELF,bs*nvertices,0,1,&islocal);CHKERRQ(ierr);
#if defined(INTERLACING) 
#if defined(BLOCKING)
   ICALLOC(nvertices,&svertices);
   for (i=0; i < nvertices; i++) 
       svertices[i] = bs*loc2pet[i];
   ierr = ISCreateBlock(MPI_COMM_SELF,bs,nvertices,svertices,&isglobal);CHKERRQ(ierr);
#else
   ICALLOC(bs*nvertices,&svertices);
   for (i = 0; i < nvertices; i++)
     for (j = 0; j < bs; j++)
       svertices[j+bs*i] = j + bs*loc2pet[i];
   ierr = ISCreateGeneral(MPI_COMM_SELF,bs*nvertices,svertices,&isglobal);CHKERRQ(ierr);
#endif
#else
   ICALLOC(bs*nvertices,&svertices);
   for (j = 0; j < bs; j++)
    for (i = 0; i < nvertices; i++)
       svertices[j*nvertices+i] = j*nvertices + loc2pet[i];
   ierr = ISCreateGeneral(MPI_COMM_SELF,bs*nvertices,svertices,&isglobal);CHKERRQ(ierr);
#endif
   ierr = PetscFree(svertices);CHKERRQ(ierr);
   ierr = VecScatterCreate(grid->qnode,isglobal,grid->qnodeLoc,islocal,&grid->scatter);CHKERRQ(ierr);
   ierr = ISDestroy(isglobal);CHKERRQ(ierr);
   ierr = ISDestroy(islocal);CHKERRQ(ierr);

/* Now create scatter for gradient vector of qnode */
   ierr = ISCreateStride(MPI_COMM_SELF,3*bs*nvertices,0,1,&islocal);CHKERRQ(ierr);
#if defined(INTERLACING)
#if defined(BLOCKING)
   ICALLOC(nvertices,&svertices);
   for (i=0; i < nvertices; i++)
       svertices[i] = 3*bs*loc2pet[i];
   ierr = ISCreateBlock(MPI_COMM_SELF,3*bs,nvertices,svertices,&isglobal);CHKERRQ(ierr);
#else
   ICALLOC(3*bs*nvertices,&svertices);
   for (i = 0; i < nvertices; i++)
     for (j = 0; j < 3*bs; j++)
       svertices[j+3*bs*i] = j + 3*bs*loc2pet[i];
   ierr = ISCreateGeneral(MPI_COMM_SELF,3*bs*nvertices,svertices,&isglobal);CHKERRQ(ierr);
#endif
#else
   ICALLOC(3*bs*nvertices,&svertices);
   for (j = 0; j < 3*bs; j++)
    for (i = 0; i < nvertices; i++)
       svertices[j*nvertices+i] = j*nvertices + loc2pet[i];
   ierr = ISCreateGeneral(MPI_COMM_SELF,3*bs*nvertices,svertices,&isglobal);CHKERRQ(ierr);
#endif
   ierr = PetscFree(svertices);
   ierr = VecScatterCreate(grid->grad,isglobal,grid->gradLoc,islocal,&grid->gradScatter);CHKERRQ(ierr);
   ierr = ISDestroy(isglobal);CHKERRQ(ierr);
   ierr = ISDestroy(islocal);CHKERRQ(ierr);

/* Store the number of non-zeroes per row */
#if defined(INTERLACING)
#if defined(BLOCKING)
   ICALLOC(nnodesLoc,&val_diag);
   ICALLOC(nnodesLoc,&val_offd);
   for (i = 0; i < nnodesLoc; i++) {
    jstart = grid->ia[i] - 1;
    jend = grid->ia[i+1] - 1;
    nbrs_diag = 0;
    nbrs_offd = 0;
    for (j = jstart; j < jend; j++) {
      if ((grid->ja[j] >= rstart) && (grid->ja[j] < (rstart+nnodesLoc)))
         nbrs_diag++;
      else
         nbrs_offd++;
    }
    val_diag[i] = nbrs_diag; 
    val_offd[i] = nbrs_offd; 
   }
   ierr = MatCreateMPIBAIJ(comm,bs,bs*nnodesLoc,bs*nnodesLoc,
                             bs*nnodes,bs*nnodes,PETSC_NULL,val_diag,
                             PETSC_NULL,val_offd,&grid->A);CHKERRQ(ierr);
#else
   ICALLOC(nnodesLoc*4,&val_diag);
   ICALLOC(nnodesLoc*4,&val_offd);
   for (i = 0; i < nnodesLoc; i++) {
    jstart = grid->ia[i] - 1;
    jend = grid->ia[i+1] - 1;
    nbrs_diag = 0;
    nbrs_offd = 0;
    for (j = jstart; j < jend; j++) {
      if ((grid->ja[j] >= rstart) && (grid->ja[j] < (rstart+nnodesLoc)))
         nbrs_diag++;
      else
         nbrs_offd++;
    }
    for (j = 0; j < 4; j++) {
      row = 4*i + j;
      val_diag[row] = nbrs_diag*4; 
      val_offd[row] = nbrs_offd*4; 
    }
   }
   ierr = MatCreateMPIAIJ(comm,bs*nnodesLoc,bs*nnodesLoc,
                             bs*nnodes,bs*nnodes,PETSC_NULL,val_diag,
                             PETSC_NULL,val_offd,&grid->A);CHKERRQ(ierr);
#endif
   ierr = PetscFree(val_diag);CHKERRQ(ierr);
   ierr = PetscFree(val_offd);CHKERRQ(ierr);

#else
   if (size > 1) {
     SETERRQ(1,1,"Parallel case not supported in non-interlaced case\n");
   }
   ICALLOC(nnodes*4,&val_diag);
   ICALLOC(nnodes*4,&val_offd);
   for (j = 0; j < 4; j++) {
    for (i = 0; i < nnodes; i++) {
      row = i + j*nnodes;
      jstart = grid->ia[i] - 1;
      jend = grid->ia[i+1] - 1;
      nbrs_diag = jend - jstart;
      val_diag[row] = nbrs_diag*4;
      val_offd[row] = 0;
    }
   }
   /* ierr = MatCreateSeqAIJ(MPI_COMM_SELF,nnodes*4,nnodes*4,PETSC_NULL,
                          val,&grid->A);*/
   ierr = MatCreateMPIAIJ(comm,bs*nnodesLoc,bs*nnodesLoc,
                             bs*nnodes,bs*nnodes,PETSC_NULL,val_diag,
                             PETSC_NULL,val_offd,&grid->A);CHKERRQ(ierr);
   ierr = PetscFree(val_diag);CHKERRQ(ierr);
   ierr = PetscFree(val_offd);CHKERRQ(ierr);
#endif

   ierr = OptionsHasName(0,"-mem_use",&flg);CHKERRQ(ierr);
   if (flg) {
    ierr = PetscShowMemoryUsage(VIEWER_STDOUT_WORLD,"Memory usage after allocating PETSc data structures\n");CHKERRQ(ierr);
   }

/* Set local to global mapping for setting the matrix elements in
 * local ordering : first set row by row mapping
 */
#if defined(INTERLACING)
   ICALLOC(bs*nvertices,&svertices);
   k = 0;
   for (i=0; i < nvertices; i++)
     for (j=0; j < bs; j++)
       svertices[k++] = (bs*loc2pet[i] + j);
   /*ierr = MatSetLocalToGlobalMapping(grid->A,bs*nvertices,svertices);CHKERRQ(ierr);*/
   ierr = ISLocalToGlobalMappingCreate(MPI_COMM_SELF,bs*nvertices,svertices,&isl2g);
   ierr = MatSetLocalToGlobalMapping(grid->A,isl2g);CHKERRQ(ierr);
   ierr = ISLocalToGlobalMappingDestroy(isl2g);CHKERRQ(ierr);

/* Now set the blockwise local to global mapping */
#if defined(BLOCKING)
   /*ierr = MatSetLocalToGlobalMappingBlock(grid->A,nvertices,loc2pet);CHKERRQ(ierr);*/
   ierr = ISLocalToGlobalMappingCreate(MPI_COMM_SELF,nvertices,loc2pet,&isl2g);
   ierr = MatSetLocalToGlobalMappingBlock(grid->A,isl2g);CHKERRQ(ierr);
   ierr = ISLocalToGlobalMappingDestroy(isl2g);CHKERRQ(ierr);
#endif
   ierr = PetscFree(svertices);CHKERRQ(ierr);
#endif
   /*ierr = MatSetOption(grid->A,MAT_COLUMNS_SORTED);CHKERRQ(ierr);*/

   PetscFunctionReturn(0);
}

/*================================= CLINK ===================================*/
/*                                                                           */
/* Used in establishing the links between FORTRAN common blocks and C        */
/*                                                                           */
/*===========================================================================*/
EXTERN_C_BEGIN
#undef __FUNC__
#define __FUNC__ "f77CLINK"
void f77CLINK(CINFO *p1,CRUNGE *p2,CGMCOM *p3)
{
   c_info  = p1;
   c_runge = p2;
   c_gmcom = p3;
}
EXTERN_C_END

/*========================== SET_UP_GRID====================================*/
/*                                                                          */
/* Allocates the memory for the fine grid                                   */
/*                                                                          */
/*==========================================================================*/
#undef __FUNC__
#define __FUNC__ "set_up_grid"
int set_up_grid(GRID *grid)                           
{
   int nnodes,nedge;
   int nsface,nvface,nfface,nbface;
   int tnode;
   /*int vface,lnodes,nnz,ncell,kvisc,ilu0,nsrch,ileast,ifcn,valloc;*/
   /*int nsnode,nvnode,nfnode; */
   /*int mgzero=0;*/ /* Variable so we dont allocate memory for multigrid */
   /*int jalloc;*/  /* If jalloc=1 allocate space for dfp and dfm */
/*
 * stuff to read in dave's grids
 */
   /*int nnbound,nvbound,nfbound,nnfacet,nvfacet,nffacet,ntte;*/
/* end of stuff */

   PetscFunctionBegin;
   nnodes = grid->nnodes;
   tnode  = grid->nnodes;
   nedge  = grid->nedge;
   nsface = grid->nsface;
   nvface = grid->nvface;
   nfface = grid->nfface;
   nbface = nsface + nvface + nfface;
 
   /*ncell  = grid->ncell;
   vface  = grid->nedge;
   lnodes = grid->nnodes;
   nsnode = grid->nsnode;
   nvnode = grid->nvnode;
   nfnode = grid->nfnode;
   nsrch  = c_gmcom->nsrch;
   ilu0   = c_gmcom->ilu0;
   ileast = grid->ileast;
   ifcn   = c_gmcom->ifcn;
   jalloc = 0;
   kvisc  = grid->jvisc;*/

   /*if(ilu0 >=1 && ifcn == 1)jalloc=0;*/

/*
 * stuff to read in dave's grids
 */
   /*nnbound = grid->nnbound;
   nvbound = grid->nvbound;
   nfbound = grid->nfbound;
   nnfacet = grid->nnfacet;
   nvfacet = grid->nvfacet;
   nffacet = grid->nffacet;
   ntte    = grid->ntte;*/
/* end of stuff */
   

   /*if (ileast == 0) lnodes = 1;
     printf("In set_up_grid->jvisc = %d\n",grid->jvisc);

   if (grid->jvisc != 2 && grid->jvisc != 4 && grid->jvisc != 6)vface = 1;
   printf(" vface = %d \n",vface); 
   if (grid->jvisc < 3) tnode = 1;
   valloc = 1;
   if (grid->jvisc ==  0)valloc = 0;*/

/*PetscPrintf(PETSC_COMM_WORLD," nsnode= %d nvnode= %d nfnode= %d\n",nsnode,nvnode,nfnode);*/
/*PetscPrintf(PETSC_COMM_WORLD," nsface= %d nvface= %d nfface= %d\n",nsface,nvface,nfface);
PetscPrintf(PETSC_COMM_WORLD," nbface= %d\n",nbface);*/
/* Now allocate memory for the other grid arrays */
/* ICALLOC(nedge*2,  &grid->eptr); */
   ICALLOC(nsface,   &grid->isface);
   ICALLOC(nvface,   &grid->ivface);
   ICALLOC(nfface,   &grid->ifface);
/* ICALLOC(nsnode,   &grid->isnode);
   ICALLOC(nvnode,   &grid->ivnode);
   ICALLOC(nfnode,   &grid->ifnode);*/
   /*ICALLOC(nnodes,   &grid->clist);
   ICALLOC(nnodes,   &grid->iupdate);
   ICALLOC(nsface*2, &grid->sface);
   ICALLOC(nvface*2, &grid->vface);
   ICALLOC(nfface*2, &grid->fface);
   ICALLOC(lnodes,   &grid->icount);*/
   /*FCALLOC(nnodes,   &grid->x);
   FCALLOC(nnodes,   &grid->y);
   FCALLOC(nnodes,   &grid->z);
   FCALLOC(nnodes,   &grid->area);*/
/*
 * FCALLOC(nnodes*4, &grid->gradx);
 * FCALLOC(nnodes*4, &grid->grady);
 * FCALLOC(nnodes*4, &grid->gradz);
 * FCALLOC(nnodes,   &grid->cdt);
 */
/*
 * FCALLOC(nnodes*4, &grid->qnode);
 * FCALLOC(nnodes*4, &grid->dq);
 * FCALLOC(nnodes*4, &grid->res);
 * FCALLOC(jalloc*nnodes*4*4,&grid->A);
 * FCALLOC(nnodes*4,  &grid->B);
 * FCALLOC(jalloc*nedge*4*4,&grid->dfp);
 * FCALLOC(jalloc*nedge*4*4,&grid->dfm);
 */
   /*FCALLOC(nsnode,   &grid->sxn);
   FCALLOC(nsnode,   &grid->syn);
   FCALLOC(nsnode,   &grid->szn);
   FCALLOC(nsnode,   &grid->sa);
   FCALLOC(nvnode,   &grid->vxn);
   FCALLOC(nvnode,   &grid->vyn);
   FCALLOC(nvnode,   &grid->vzn);
   FCALLOC(nvnode,   &grid->va);
   FCALLOC(nfnode,   &grid->fxn);
   FCALLOC(nfnode,   &grid->fyn);
   FCALLOC(nfnode,   &grid->fzn);
   FCALLOC(nfnode,   &grid->fa);
   FCALLOC(nedge,    &grid->xn);
   FCALLOC(nedge,    &grid->yn);
   FCALLOC(nedge,    &grid->zn);
   FCALLOC(nedge,    &grid->rl);*/

   FCALLOC(nbface*15,&grid->us);
   FCALLOC(nbface*15,&grid->vs);
   FCALLOC(nbface*15,&grid->as);
/*
 * FCALLOC(nnodes*4, &grid->phi);
 * FCALLOC(nnodes,   &grid->r11);
 * FCALLOC(nnodes,   &grid->r12);
 * FCALLOC(nnodes,   &grid->r13);
 * FCALLOC(nnodes,   &grid->r22);
 * FCALLOC(nnodes,   &grid->r23);
 * FCALLOC(nnodes,   &grid->r33);
 */
/*
 * Allocate memory for viscous length scale if turbulent 
 */
   if (grid->jvisc >= 3) {
    FCALLOC(tnode,  &grid->slen);
    FCALLOC(nnodes, &grid->turbre);
    FCALLOC(nnodes, &grid->amut);
    FCALLOC(tnode,  &grid->turbres);
    FCALLOC(nedge,  &grid->dft1);
    FCALLOC(nedge,  &grid->dft2);
   }
/*
 * Allocate memory for MG transfer 
 */
/*
 * ICALLOC(mgzero*nsface,   &grid->isford);
 * ICALLOC(mgzero*nvface,   &grid->ivford);
 * ICALLOC(mgzero*nfface,   &grid->ifford);
 * ICALLOC(mgzero*nnodes,   &grid->nflag);
 * ICALLOC(mgzero*nnodes,   &grid->nnext);
 * ICALLOC(mgzero*nnodes,   &grid->nneigh);
 * ICALLOC(mgzero*ncell,    &grid->ctag);
 * ICALLOC(mgzero*ncell,    &grid->csearch);
 * ICALLOC(valloc*ncell*4,  &grid->c2n);
 * ICALLOC(valloc*ncell*6,  &grid->c2e);
 * grid->c2c = (int *)grid->dfp;
 * ICALLOC(ncell*4,  &grid->c2c); 
 * ICALLOC(nnodes,   &grid->cenc);
 * if (grid->iup == 1) {
 *    ICALLOC(mgzero*nnodes*3, &grid->icoefup);
 *    FCALLOC(mgzero*nnodes*3, &grid->rcoefup);
 * }
 * if (grid->idown == 1) {
 *    ICALLOC(mgzero*nnodes*3, &grid->icoefdn);
 *    FCALLOC(mgzero*nnodes*3, &grid->rcoefdn);
 * }
 * FCALLOC(nnodes*4, &grid->ff);
 * FCALLOC(tnode,    &grid->turbff);
 */
/*
 * If using GMRES (nsrch>0) allocate memory
 */
/* NoEq = 0;
*  if(nsrch > 0)NoEq = 4*nnodes;
*  if(nsrch < 0)NoEq = nnodes;
*  FCALLOC(NoEq,          &grid->AP);
*  FCALLOC(NoEq,          &grid->Xgm);
*  FCALLOC(NoEq,          &grid->temr);
*  FCALLOC((abs(nsrch)+1)*NoEq,&grid->Fgm);
*/
/*
 * stuff to read in dave's grids
 */
/*
 * ICALLOC(nnbound,  &grid->ncolorn);
 * ICALLOC(nnbound*100,&grid->countn);
 * ICALLOC(nvbound,  &grid->ncolorv);
 * ICALLOC(nvbound*100,&grid->countv);
 * ICALLOC(nfbound,  &grid->ncolorf);
 * ICALLOC(nfbound*100,&grid->countf);
 */
   /*ICALLOC(nnbound,  &grid->nntet);
   ICALLOC(nnbound,  &grid->nnpts);
   ICALLOC(nvbound,  &grid->nvtet);
   ICALLOC(nvbound,  &grid->nvpts);
   ICALLOC(nfbound,  &grid->nftet);
   ICALLOC(nfbound,  &grid->nfpts);
   ICALLOC(nnfacet*4,&grid->f2ntn);
   ICALLOC(nvfacet*4,&grid->f2ntv);
   ICALLOC(nffacet*4,&grid->f2ntf);*/
  PetscFunctionReturn(0);
}

 
/*========================== WRITE_FINE_GRID ================================*/
/*                                                                           */
/* Write memory locations and other information for the fine grid            */
/*                                                                           */
/*===========================================================================*/
#undef __FUNC__
#define __FUNC__ "write_fine_grid"
int write_fine_grid(GRID *grid)                  
{
   FILE *output;

   PetscFunctionBegin;
/* open file for output      */
/* call the output frame.out */

   if (!(output = fopen("frame.out","a"))){
      SETERRQ(1,1,"can't open frame.out");
   }
   fprintf(output,"information for fine grid\n"); 
   fprintf(output,"\n");
   fprintf(output," address of fine grid = %p\n",grid);

   fprintf(output,"grid.nnodes  = %d\n",grid->nnodes);
   fprintf(output,"grid.ncell   = %d\n",grid->ncell);
   fprintf(output,"grid.nedge   = %d\n",grid->nedge);
   fprintf(output,"grid.nsface  = %d\n",grid->nsface);
   fprintf(output,"grid.nvface  = %d\n",grid->nvface);
   fprintf(output,"grid.nfface  = %d\n",grid->nfface);
   fprintf(output,"grid.nsnode  = %d\n",grid->nsnode);
   fprintf(output,"grid.nvnode  = %d\n",grid->nvnode);
   fprintf(output,"grid.nfnode  = %d\n",grid->nfnode);

   fprintf(output,"grid.eptr    = %p\n",grid->eptr);
   fprintf(output,"grid.isface  = %p\n",grid->isface);
   fprintf(output,"grid.ivface  = %p\n",grid->ivface);
   fprintf(output,"grid.ifface  = %p\n",grid->ifface);
   fprintf(output,"grid.isnode  = %p\n",grid->isnode);
   fprintf(output,"grid.ivnode  = %p\n",grid->ivnode);
   fprintf(output,"grid.ifnode  = %p\n",grid->ifnode);
   fprintf(output,"grid.c2n     = %p\n",grid->c2n);
   fprintf(output,"grid.c2e     = %p\n",grid->c2e);
   fprintf(output,"grid.xyz     = %p\n",grid->xyz);
   /*fprintf(output,"grid.y       = %p\n",grid->xyz);
     fprintf(output,"grid.z       = %p\n",grid->z);*/
   fprintf(output,"grid.area    = %p\n",grid->area);
   fprintf(output,"grid.qnode   = %p\n",grid->qnode);
/*
   fprintf(output,"grid.gradx   = %p\n",grid->gradx);
   fprintf(output,"grid.grady   = %p\n",grid->grady);
   fprintf(output,"grid.gradz   = %p\n",grid->gradz);
*/
   fprintf(output,"grid.cdt     = %p\n",grid->cdt);
   fprintf(output,"grid.sxn     = %p\n",grid->sxn);
   fprintf(output,"grid.syn     = %p\n",grid->syn);
   fprintf(output,"grid.szn     = %p\n",grid->szn);
   fprintf(output,"grid.vxn     = %p\n",grid->vxn);
   fprintf(output,"grid.vyn     = %p\n",grid->vyn);
   fprintf(output,"grid.vzn     = %p\n",grid->vzn);
   fprintf(output,"grid.fxn     = %p\n",grid->fxn);
   fprintf(output,"grid.fyn     = %p\n",grid->fyn);
   fprintf(output,"grid.fzn     = %p\n",grid->fzn);
   fprintf(output,"grid.xyzn    = %p\n",grid->xyzn);
   /*fprintf(output,"grid.yn      = %p\n",grid->yn);
   fprintf(output,"grid.zn      = %p\n",grid->zn);
   fprintf(output,"grid.rl      = %p\n",grid->rl);*/
/*
 * close output file
 */
   fclose(output);
   PetscFunctionReturn(0);
}

#if defined (PARCH_IRIX64) && defined(USE_HW_COUNTERS)
int EventCountersBegin(int *gen_start,Scalar* time_start_counters)
{
 int ierr;
 if ((*gen_start = start_counters(event0,event1)) < 0)
   SETERRQ(1,1,"Error in start_counters\n");
 ierr = PetscGetTime(time_start_counters);CHKERRQ(ierr);
 return 0;
}

int EventCountersEnd(int gen_start,Scalar time_start_counters) 
{
 int gen_read,ierr;
 Scalar time_read_counters;
 long long _counter0,_counter1;

 if ((gen_read = read_counters(event0,&_counter0,event1,&_counter1)) < 0)
   SETERRQ(1,1,"Error in read_counter\n");
 ierr = PetscGetTime(&time_read_counters);CHKERRQ(ierr);
 if (gen_read != gen_start) {
   SETERRQ(1,1,"Lost Counters!! Aborting ...\n");
 }
 counter0 += _counter0;
 counter1 += _counter1;
 time_counters += time_read_counters-time_start_counters;
 return 0;
}
#endif

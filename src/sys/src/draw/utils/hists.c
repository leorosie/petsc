
#ifdef PETSC_RCS_HEADER
static char vcid[] = "$Id: lg.c,v 1.1.1.1 1997/08/23 06:21:01 knepley Exp $";
#endif

/*
  Contains the data structure for plotting a histogram in a window with an axis.
*/

#include "petsc.h"         /*I "petsc.h" I*/

struct _p_DrawHist {
  PETSCHEADER 
  Draw     win;
  DrawAxis axis;
  double   xmin, xmax;
  double   ymin, ymax;
  int      numBins;
  double   *bins;
  int      numValues;
  int      maxValues;
  double   *values;
  int      color;
};

#define CHUNKSIZE 100

#undef __FUNC__  
#define __FUNC__ "DrawHistCreate"
/*@C
  DrawHistCreate - Creates a histogram data structure.

  Input Parameters:
. win  - The window where the graph will be made
. bins - The number of bins to use

  Output Parameters:
. hist - The histogram context

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, create

.seealso: DrawHistDestroy()

@*/
int DrawHistCreate(Draw win, int bins, DrawHist *hist)
{
  int         ierr;
  PetscObject vobj = (PetscObject) win;
  DrawHist    h;

  if (vobj->cookie == DRAW_COOKIE && vobj->type == DRAW_NULLWINDOW) {
    ierr = DrawOpenNull(vobj->comm, (Draw *) hist); CHKERRQ(ierr);
    (*hist)->win = win;
    return(0);
  }
  PetscHeaderCreate(h, _p_DrawHist, DRAW_COOKIE, 0, vobj->comm, DrawHistDestroy, 0);
  h->view      = 0;
  h->destroy   = 0;
  h->win       = win;
  h->color     = DRAW_GREEN;
  h->xmin      = PETSC_MAX;
  h->xmax      = PETSC_MIN;
  h->ymin      = 0.;
  h->ymax      = 1.;
  h->numBins   = bins;
  h->bins      = (double *)    PetscMalloc(bins*sizeof(double));    CHKPTRQ(h->bins);
  h->numValues = 0;
  h->maxValues = CHUNKSIZE;
  h->values    = (double *) PetscMalloc(h->maxValues * sizeof(double)); CHKPTRQ(h->values);
  PLogObjectMemory(h, bins*sizeof(double) + h->maxValues*sizeof(double));
  ierr = DrawAxisCreate(win, &h->axis); CHKERRQ(ierr);
  PLogObjectParent(h, h->axis);
  *hist = h;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistSetNumberBins"
/*@
  DrawHistSetNumberBins - Change the number of bins that are to be drawn.

  Input Parameter:
. hist - The histogram context.
. dim  - The number of curves.

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, create

@*/
int DrawHistSetNumberBins(DrawHist hist, int bins)
{
  int ierr;

  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);

  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  if (hist->numBins == bins) return(0);

  ierr          = PetscFree(hist->bins); CHKERRQ(ierr);
  hist->bins    = (double *) PetscMalloc(bins*sizeof(double)); CHKPTRQ(hist->bins);
  PLogObjectMemory(hist, (bins - hist->numBins) * sizeof(double));
  hist->numBins = bins;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistReset"
/*@
  DrawHistReset - Clears histogram to allow for reuse with new data.

  Input Parameter:
. hist - The histogram context.

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, reset
@*/
int DrawHistReset(DrawHist hist)
{
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);
  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  hist->xmin      = PETSC_MAX;
  hist->xmax      = PETSC_MIN;
  hist->ymin      = 0;
  hist->ymax      = 0;
  hist->numValues = 0;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistDestroy"
/*@C
  DrawHistDestroy - Frees all space taken up by histogram data structure.

  Input Parameter:
. hist - The histogram context

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, destroy

.seealso:  DrawHistCreate()
@*/
int DrawHistDestroy(DrawHist hist)
{
  int ierr;

  if (!hist || !(hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW)) {
    PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  }

  if (--hist->refct > 0) return(0);
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) {
    return(PetscObjectDestroy((PetscObject) hist));
  }

  ierr = DrawAxisDestroy(hist->axis); CHKERRQ(ierr);
  ierr = PetscFree(hist->bins);       CHKERRQ(ierr);
  ierr = PetscFree(hist->values);     CHKERRQ(ierr);
  PLogObjectDestroy(hist);
  PetscHeaderDestroy(hist);
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistAddValue"
/*@
  DrawHistAddValue - Adds another value to the histogram.

  Input Parameters:
. hist  - The histogram
. value - The value 

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, add, point

.seealso: DrawHistAddValues()
@*/
int DrawHistAddValue(DrawHist hist, double value)
{
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);

  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  /* Allocate more memory if necessary */
  if (hist->numValues >= hist->maxValues) {
    double *tmp;
    int     ierr;

    tmp = (double *) PetscMalloc((hist->maxValues + CHUNKSIZE) * sizeof(double));CHKPTRQ(tmp);
    PLogObjectMemory(hist, CHUNKSIZE * sizeof(double));
    PetscMemcpy(tmp, hist->values, hist->maxValues * sizeof(double));
    ierr = PetscFree(hist->values); CHKERRQ(ierr);
    hist->values     = tmp;
    hist->maxValues += CHUNKSIZE;
  }
  if (hist->numValues == 0) {
    hist->xmin = value;
    hist->xmax = value;
  } else if (hist->numValues == 1) {
    /* Update limits -- We need to overshoot the largest value somewhat */
    if (value > hist->xmax)
      hist->xmax = value + 0.001*(value - hist->xmin)/hist->numBins;
    if (value < hist->xmin)
    {
      hist->xmin = value;
      hist->xmax = hist->xmax + 0.001*(hist->xmax - hist->xmin)/hist->numBins;
    }
  } else {
    /* Update limits -- We need to overshoot the largest value somewhat */
    if (value > hist->xmax) {
      hist->xmax = value + 0.001*(hist->xmax - hist->xmin)/hist->numBins;
    }
    if (value < hist->xmin) {
      hist->xmin = value;
    }
  }

  hist->values[hist->numValues++] = value;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistDraw"
/*@
  DrawHistDraw - Redraws a histogram.

  Input Parameter:
. hist - The histogram context

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph
@*/
int DrawHistDraw(DrawHist hist)
{
  Draw     win;
  double   xmin,xmax,ymin,ymax,*bins,*values,binSize,binLeft, binRight,maxHeight;
  int      numBins,numValues,i, p,ierr,bcolor, color;;

  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);
  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  if ((hist->xmin >= hist->xmax) || (hist->ymin >= hist->ymax)) return(0);
  if (hist->numValues < 1) return(0);

  color = hist->color; 
  if (color == DRAW_ROTATE) {bcolor = 2;} else {bcolor = color;}
  win       = hist->win;
  xmin      = hist->xmin;
  xmax      = hist->xmax;
  ymin      = hist->ymin;
  ymax      = hist->ymax;
  numBins   = hist->numBins;
  bins      = hist->bins;
  numValues = hist->numValues;
  values    = hist->values;
  binSize   = (xmax - xmin)/numBins;

  ierr = DrawClear(win);                                        CHKERRQ(ierr);
  /* Calculate number of points in each bin */
  PetscMemzero(bins, numBins * sizeof(double));
  maxHeight = 0;
  for (i = 0; i < numBins; i++) {
    binLeft   = xmin + binSize*i;
    binRight  = xmin + binSize*(i+1);
    for(p = 0; p < numValues; p++) {
      if ((values[p] >= binLeft) && (values[p] < binRight)) bins[i]++;
    }
    maxHeight = PetscMax(maxHeight, bins[i]);
  }
  if (maxHeight > ymax) ymax = hist->ymax = maxHeight;
  ierr = DrawAxisSetLimits(hist->axis, xmin, xmax, ymin, ymax); CHKERRQ(ierr);
  ierr = DrawAxisDraw(hist->axis);                              CHKERRQ(ierr);
  /* Draw bins */
  for (i = 0; i < numBins; i++) {
    binLeft   = xmin + binSize*i;
    binRight  = xmin + binSize*(i+1);
    ierr = DrawRectangle(win,binLeft,ymin,binRight,bins[i],bcolor,bcolor,bcolor,bcolor);CHKERRQ(ierr);
    if (color == DRAW_ROTATE && bins[i]) bcolor++; if (bcolor > 31) bcolor = 2;
    ierr = DrawLine(win,binLeft,ymin,binLeft,bins[i],DRAW_BLACK);CHKERRQ(ierr);
    ierr = DrawLine(win,binRight,ymin,binRight,bins[i],DRAW_BLACK); CHKERRQ(ierr);
    ierr = DrawLine(win,binLeft,bins[i],binRight,bins[i],DRAW_BLACK);CHKERRQ(ierr);
  }
  DrawSyncFlush(win);
  DrawPause(win);
  return(0);
} 
 
#undef __FUNC__  
#define __FUNC__ "DrawHistSetColor"
/*@
  DrawHistSetColor - Sets the color the bars will be drawn with.

  Input Parameters:
. hist - The histogram context
. color - one of the colors defined in draw.h or DRAW_ROTATE to make each bar a 
          different color

.keywords:  draw, histogram, graph, color

@*/
int DrawHistSetColor(DrawHist hist, int color)
{
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);
  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  hist->color = color;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistSetLimits"
/*@
  DrawHistSetLimits - Sets the axis limits for a histogram. If more
  points are added after this call, the limits will be adjusted to
  include those additional points.

  Input Parameters:
. hist - The histogram context
. x_min,x_max,y_min,y_max - The limits

  Contributed by: Matthew Knepley

.keywords:  draw, histogram, graph, set limits

@*/
int DrawHistSetLimits(DrawHist hist, double x_min, double x_max, int y_min, int y_max) 
{
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) return(0);
  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  hist->xmin = x_min; 
  hist->xmax = x_max; 
  hist->ymin = y_min; 
  hist->ymax = y_max;
  return(0);
}
 
#undef __FUNC__  
#define __FUNC__ "DrawHistGetAxis"
/*@C
  DrawHistGetAxis - Gets the axis context associated with a histogram.
  This is useful if one wants to change some axis property, such as
  labels, color, etc. The axis context should not be destroyed by the
  application code.

  Input Parameter:
. hist - The histogram context

  Output Parameter:
. axis - The axis context

  Contributed by: Matthew Knepley

.keywords: draw, histogram, graph, get, axis

@*/
int DrawHistGetAxis(DrawHist hist, DrawAxis *axis)
{
  if (hist && hist->cookie == DRAW_COOKIE && hist->type == DRAW_NULLWINDOW) {
    *axis = 0;
    return(0);
  }
  PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  *axis = hist->axis;
  return(0);
}

#undef __FUNC__  
#define __FUNC__ "DrawHistGetDraw"
/*@C
  DrawHistGetDraw - Gets the draw context associated with a histogram.

  Input Parameter:
. hist - The histogram context

  Output Parameter:
. win  - The draw context

  Contributed by: Matthew Knepley

.keywords: draw, histogram, graph, get, context

@*/
int DrawHistGetDraw(DrawHist hist, Draw *win)
{
  if (!hist || hist->cookie != DRAW_COOKIE || hist->type != DRAW_NULLWINDOW) {
    PetscValidHeaderSpecific(hist, DRAW_COOKIE);
  }
  *win = hist->win;
  return(0);
}


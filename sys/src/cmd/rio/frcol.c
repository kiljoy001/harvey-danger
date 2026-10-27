#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <frame.h>

void
frsetcol(Frame *f, Image *text, Image *back)
{
	/* f->coltxt = f->cols[TEXT]; */
	/* f->colbak = f->cols[BACK]; */
	if(text != nil)
		f->cols[TEXT] = text;
	if(back != nil)
		f->cols[BACK] = back;
}

/* void */
/* frresetcol(Frame *f) */
/* { */
/* 	if(f->coltxt != nil){ */
/* 		f->cols[TEXT] = f->coltxt; */
/* 		f->coltxt = nil; */
/* 	} */
/* 	if(f->colbak != nil){ */
/* 		f->cols[BACK] = f->colbak; */
/* 		f->colbak = nil; */
/* 	} */
/* } */

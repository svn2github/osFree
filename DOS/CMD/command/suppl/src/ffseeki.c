/*
    This file is part of SUPPL - the supplemental library for DOS
    Copyright (C) 1996-2000 Steffen Kaiser

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
/* $RCSfile$
   $Locker$	$Name$	$State$

ob(ject): Fseeki
su(bsystem): supplio
ty(pe): 
sh(ort description): Seek a stream relatively by an \tok{(int)} offset
pr(erequistes): fp != NULL
va(lue): 0: on success\item else: on failure
re(lated to): Fseek
se(condary subsystems): 
in(itialized by): 
wa(rning): 
bu(gs): 
co(mpilers): Micro-C only

*/

#include "initsupl.loc"

#ifdef _MICROC_
#include <stdio.h>
#include <portable.h>
#include "supplio.h"

#include "suppldbg.h"

#ifdef RCS_Version
static char const rcsid[] = 
	"$Id: ffseeki.c 1210 2006-06-17 03:25:06Z blairdude $";
#endif

int Fseeki(FILE *fp, int ofs)
{	DBG_ENTER("Fseeki", Suppl_supplio)
	assert(fp);
	DBG_RETURN_BI( fseek(fp, ofs < 0? -1: 0, ofs, SEEK_CUR))
}

#endif

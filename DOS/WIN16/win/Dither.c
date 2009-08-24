/*    
	Dither.c	2.11
    	Copyright 1997 Willows Software, Inc. 

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public License as
published by the Free Software Foundation; either version 2 of the
License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.

You should have received a copy of the GNU Library General Public
License along with this library; see the file COPYING.LIB.  If
not, write to the Free Software Foundation, Inc., 675 Mass Ave,
Cambridge, MA 02139, USA.


For more information about the Willows Twin Libraries.

	http://www.willows.com	

To send email to the maintainer of the Willows Twin Libraries.

	mailto:twin@willows.com 

 */

#include <string.h>
#include "windows.h"
#include "Endian.h"

#define	DM_DEFAULT	0x00000001	/* for dithering routine */
#define	DM_MONOCHROME	0x00000004

/**************************************************************************\
* This function takes a value from 0 - 255 and uses it to create an
* 8x8 pile of bits in the form of a 1BPP bitmap.  It can also take an
* RGB value and make an 8x8 bitmap.  These can then be used as brushes
* to simulate color unavaible on the device.
*
* For monochrome the basic algorithm is equivalent to turning on bits
* in the 8x8 array according to the following order:
*
*  00 32 08 40 02 34 10 42
*  48 16 56 24 50 18 58 26
*  12 44 04 36 14 46 06 38
*  60 28 52 20 62 30 54 22
*  03 35 11 43 01 33 09 41
*  51 19 59 27 49 17 57 25
*  15 47 07 39 13 45 05 37
*  63 31 55 23 61 29 53 21
*
* Reference: A Survey of Techniques for the Display of Continous
*            Tone Pictures on Bilevel Displays,;
*            Jarvis, Judice, & Ninke;
*            COMPUTER GRAPHICS AND IMAGE PROCESSING 5, pp 13-40, (1976)
\**************************************************************************/

#define	IN
#define	OUT

#define SWAP_RB 0x00000004
#define SWAP_GB 0x00000002
#define SWAP_RG 0x00000001

#define SWAPTHEM(a,b) (ulTemp = a, a = b, b = ulTemp)

/* PATTERNSIZE is the number of pixels in a dither pattern. */
#define PATTERNSIZE 64

typedef union _PAL_ULONG {
    PALETTEENTRY pal;
    ULONG ul;
} PAL_ULONG;

typedef struct _VERTEX_DATA {
    ULONG ulCount;  /* # of pixels in this vertex */
    ULONG ulVertex; /* vertex # */
} VERTEX_DATA;

/* Tells which row to turn a pel on in when dithering for monochrome bitmaps. */
static BYTE ajByte[] = {
    0, 4, 0, 4, 2, 6, 2, 6,
    0, 4, 0, 4, 2, 6, 2, 6,
    1, 5, 1, 5, 3, 7, 3, 7,
    1, 5, 1, 5, 3, 7, 3, 7,
    0, 4, 0, 4, 2, 6, 2, 6,
    0, 4, 0, 4, 2, 6, 2, 6,
    1, 5, 1, 5, 3, 7, 3, 7,
    1, 5, 1, 5, 3, 7, 3, 7
};

/* The array of monochrome bits used for monc */
static BYTE ajBits[] = {
    0x80, 0x08, 0x08, 0x80, 0x20, 0x02, 0x02, 0x20,
    0x20, 0x02, 0x02, 0x20, 0x80, 0x08, 0x08, 0x80,
    0x40, 0x04, 0x04, 0x40, 0x10, 0x01, 0x01, 0x10,
    0x10, 0x01, 0x01, 0x10, 0x40, 0x04, 0x04, 0x40,
    0x40, 0x04, 0x04, 0x40, 0x10, 0x01, 0x01, 0x10,
    0x10, 0x01, 0x01, 0x10, 0x40, 0x04, 0x04, 0x40,
    0x80, 0x08, 0x08, 0x80, 0x20, 0x02, 0x02, 0x20,
    0x20, 0x02, 0x02, 0x20, 0x80, 0x08, 0x08, 0x80
};

/* Translates vertices back to the original subspace. Each row is a subspace, */
/* as encoded in ulSymmetry, and each column is a vertex between 0 and 15. */
BYTE jSwapSubSpace[8*16] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    0, 2, 1, 3, 4, 6, 5, 7, 8, 10, 9, 11, 12, 14, 13, 15,
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
    0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15,
    0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15,
    0, 2, 4, 6, 1, 3, 5, 7, 8, 10, 12, 14, 9, 11, 13, 15,
    0, 4, 1, 5, 2, 6, 3, 7, 8, 12, 9, 13, 10, 14, 11, 15,
    0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15,
};

/* Converts a nibble value in the range 0-15 to a dword value containing the */
/* nibble value packed 8 times. */
ULONG ulNibbleToDword[16] = {
    0x00000000,
    0x01010101,
    0x02020202,
    0x03030303,
    0x04040404,
    0x05050505,
    0x06060606,
    0x07070707,
    0x08080808,
    0x09090909,
    0x0A0A0A0A,
    0x0B0B0B0B,
    0x0C0C0C0C,
    0x0D0D0D0D,
    0x0E0E0E0E,
    0x0F0F0F0F
};

/* Specifies where in the dither pattern colors should be placed in order
 of increasing intensity. This is organized in the following form, for
 efficiency: every set of 8 pixels (0-7, 8-15, 16-23, ... ,56-63) is
 placed in the dither pattern in the order: 0 2 4 6 1 3 5 7. This is
 done so that two longs can be combined to put 8 pixels in DIB4 format
 at once (the first dword is shifted left 4, then the two dwords are
 ORed, to produce 0 1 2 3 4 5 6 7 order in memory), which is much faster than
 combining the output of the straight dither ordering.
 The effective dither ordering after we combine each pair of ULONGS at the
 end (the desired dither ordering) is:

  0, 36,  4, 32, 18, 54, 22, 50,
  2, 38,  6, 34, 16, 52, 20, 48,
  9, 45, 13, 41, 27, 63, 31, 59,
 11, 47, 15, 43, 25, 61, 29, 57,
  1, 37,  5, 33, 19, 55, 23, 51,
  3, 39,  7, 35, 17, 53, 21, 49,
  8, 44, 12, 40, 26, 62, 30, 58,
 10, 46, 14, 42, 24, 60, 28, 56,
*/
ULONG aulDitherOrder[] = {
  0, 34,  2, 32, 17, 51, 19, 49,
  1, 35,  3, 33, 16, 50, 18, 48,
 12, 46, 14, 44, 29, 63, 31, 61,
 13, 47, 15, 45, 28, 62, 30, 60,
  4, 38,  6, 36, 21, 55, 23, 53,
  5, 39,  7, 37, 20, 54, 22, 52,
  8, 42, 10, 40, 25, 59, 27, 57,
  9, 43, 11, 41, 24, 58, 26, 56,
};

/******************************Public*Routine******************************\
* InternalDitherColor
*
* Dithers an RGB color to an 8X8 approximation using the reserved VGA colors
*
\**************************************************************************/

DWORD
InternalDitherColor(
	IN  ULONG  iMode,
	IN  DWORD  rgb,
	OUT ULONG *pul)
{
    ULONG   ulGrey, ulRed, ulGre, ulBlu, ulSymmetry;
    ULONG   ulRedTemp, ulGreenTemp, ulBlueTemp, ulTemp;
    VERTEX_DATA vVertexData[4];
    VERTEX_DATA *pvVertexData;
    VERTEX_DATA *pvVertexDataEnd;
    ULONG  *pulTemp;
    ULONG  *pulDitherOrder;
    ULONG   ulNumPixels;
    BYTE    jColor;
    ULONG   ulColor;
    VERTEX_DATA *pvMaxVertex;
    BYTE    ajDither[16*sizeof(ULONG)];
    ULONG   ulVertex0Temp, ulVertex1Temp, ulVertex2Temp, ulVertex3Temp;
    int i;

    /* Figure out if we need a full color dither or only a monochrome dither */
    if (iMode != DM_MONOCHROME) {

        /* Full color dither */

        /* Split the color into red, green, and blue components */
        ulRedTemp   = ((PAL_ULONG *)&rgb)->pal.peRed;
        ulGreenTemp   = ((PAL_ULONG *)&rgb)->pal.peGreen;
        ulBlueTemp   = ((PAL_ULONG *)&rgb)->pal.peBlue;

        /* Sort the RGB so that the point is transformed into subspace 0, and */
        /* keep track of the swaps in ulSymmetry so we can unravel it again */
        /* later.  We want r >= g >= b (subspace 0). */
        ulSymmetry = 0;
        if (ulBlueTemp > ulRedTemp) {
            SWAPTHEM(ulBlueTemp,ulRedTemp);
            ulSymmetry = SWAP_RB;
        }

        if (ulBlueTemp > ulGreenTemp) {
            SWAPTHEM(ulBlueTemp,ulGreenTemp);
            ulSymmetry |= SWAP_GB;
        }

        if (ulGreenTemp > ulRedTemp) {
            SWAPTHEM(ulGreenTemp,ulRedTemp);
            ulSymmetry |= SWAP_RG;
        }

        ulSymmetry <<= 4;   /* for lookup purposes */

        /* Scale the values from 0-255 to 0-64. Note that the scaling is not */
        /* symmetric at the ends; this is done to match Windows 3.1 dithering */
        ulRed = (ulRedTemp + 1) >> 2;
        ulGre = (ulGreenTemp + 1) >> 2;
        ulBlu = (ulBlueTemp + 1) >> 2;

        /* Compute the subsubspace within subspace 0 in which the point lies,
         then calculate the # of pixels to dither in the colors that are the
         four vertexes of the tetrahedron bounding the color we're emulating.
         Only vertices with more than zero pixels are stored, and the
         vertices are stored in order of increasing intensity, saving us the
         need to sort them later */
        if ((ulRedTemp + ulGreenTemp) > 256) {
            /* Subsubspace 2 or 3 */
            if ((ulRedTemp + ulBlueTemp) > 256) {
                /* Subsubspace 3
                 Calculate the number of pixels per vertex, still in
                 subsubspace 3, then convert to original subspace. The pixel
                 counts and vertex numbers are matching pairs, stored in
                 ascending intensity order, skipping vertices with zero
                 pixels. The vertex intensity order for subsubspace 3 is:
                 7, 9, 0x0B, 0x0F */
                pvVertexData = vVertexData;
                if ((ulVertex0Temp = (64 - ulRed) << 1) != 0) {
                    pvVertexData->ulCount = ulVertex0Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x07];
                }
                ulVertex2Temp = ulGre - ulBlu;
                ulVertex3Temp = (ulRed - 64) + ulBlu;
                if ((ulVertex1Temp = ((PATTERNSIZE - ulVertex0Temp) -
                        ulVertex2Temp) - ulVertex3Temp) != 0) {
                    pvVertexData->ulCount = ulVertex1Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x09];
                }
                if (ulVertex2Temp != 0) {
                    pvVertexData->ulCount = ulVertex2Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x0B];
                }
                if (ulVertex3Temp != 0) {
                    pvVertexData->ulCount = ulVertex3Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x0F];
                }
            } else {
                /* Subsubspace 2
                 Calculate the number of pixels per vertex, still in
                 subsubspace 2, then convert to original subspace. The pixel
                 counts and vertex numbers are matching pairs, stored in
                 ascending intensity order, skipping vertices with zero
                 pixels. The vertex intensity order for subsubspace 2 is:
                 3, 7, 9, 0x0B */
                pvVertexData = vVertexData;
                ulVertex1Temp = ulBlu << 1;
                ulVertex2Temp = ulRed - ulGre;
                ulVertex3Temp = (ulRed - 32) + (ulGre - 32);
                if ((ulVertex0Temp = ((PATTERNSIZE - ulVertex1Temp) -
                            ulVertex2Temp) - ulVertex3Temp) != 0) {
                    pvVertexData->ulCount = ulVertex0Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x03];
                }
                if (ulVertex1Temp != 0) {
                    pvVertexData->ulCount = ulVertex1Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x07];
                }
                if (ulVertex2Temp != 0) {
                    pvVertexData->ulCount = ulVertex2Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x09];
                }
                if (ulVertex3Temp != 0) {
                    pvVertexData->ulCount = ulVertex3Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x0B];
                }
            }
        } else {
            /* Subsubspace 0 or 1 */
            if (ulRedTemp > 128) {
                /* Subsubspace 1
                 Calculate the number of pixels per vertex, still in
                 subsubspace 1, then convert to original subspace. The pixel
                 counts and vertex numbers are matching pairs, stored in
                 ascending intensity order, skipping vertices with zero
                 pixels. The vertex intensity order for subsubspace 1 is:
                 1, 3, 7, 9 */
                pvVertexData = vVertexData;
                if ((ulVertex0Temp = ((32 - ulGre) + (32 - ulRed)) << 1) != 0) {
                    pvVertexData->ulCount = ulVertex0Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x01];
                }
                ulVertex2Temp = ulBlu << 1;
                ulVertex3Temp = (ulRed - 32) << 1;
                if ((ulVertex1Temp = ((PATTERNSIZE - ulVertex0Temp) -
                        ulVertex2Temp) - ulVertex3Temp) != 0) {
                    pvVertexData->ulCount = ulVertex1Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x03];
                }
                if (ulVertex2Temp != 0) {
                    pvVertexData->ulCount = ulVertex2Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x07];
                }
                if (ulVertex3Temp != 0) {
                    pvVertexData->ulCount = ulVertex3Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x09];
                }
            } else {
                /* Subsubspace 0
                 Calculate the number of pixels per vertex, still in
                 subsubspace 0, then convert to original subspace. The pixel
                 counts and vertex numbers are matching pairs, stored in
                 ascending intensity order, skipping vertices with zero
                 pixels. The vertex intensity order for subsubspace 0 is:
                 0, 1, 3, 7 */
                pvVertexData = vVertexData;
                if ((ulVertex0Temp = (32 - ulRed) << 1) != 0) {
                    pvVertexData->ulCount = ulVertex0Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x00];
                }
                if ((ulVertex1Temp = (ulRed - ulGre) << 1) != 0) {
                    pvVertexData->ulCount = ulVertex1Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x01];
                }
                ulVertex3Temp = ulBlu << 1;
                if ((ulVertex2Temp = ((PATTERNSIZE - ulVertex0Temp) -
                        ulVertex1Temp) - ulVertex3Temp) != 0) {
                    pvVertexData->ulCount = ulVertex2Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x03];
                }
                if (ulVertex3Temp != 0) {
                    pvVertexData->ulCount = ulVertex3Temp;
                    pvVertexData++->ulVertex = jSwapSubSpace[ulSymmetry + 0x07];
                }
            }
        }

        /* Now that we have found the bounding vertices and the number of */
        /* pixels to dither for each vertex, we can create the dither pattern */

        /* Handle 1, 2, and 3 & 4 vertices per dither separately */
        ulTemp = pvVertexData - vVertexData;  
			  /* # of vertices with more than zero pixels */
        if (ulTemp > 2) {

            /* There are 3 or 4 vertices in this dither */

            if (ulTemp == 3) {

                /* There are 3 vertices in this dither */

                /* Find the vertex with the most pixels, and fill the whole */
                /* destination bitmap with that vertex's color, which is faster */
                /* than dithering it */
                if (vVertexData[1].ulCount >= vVertexData[2].ulCount) {
                    pvMaxVertex = &vVertexData[1];
                    ulTemp = vVertexData[1].ulCount;
                } else {
                    pvMaxVertex = &vVertexData[2];
                    ulTemp = vVertexData[2].ulCount;
                }

            } else {

                /* There are 4 vertices in this dither */

                /* Find the vertex with the most pixels, and fill the whole */
                /* destination bitmap with that vertex's color, which is faster */
                /* than dithering it */
                if (vVertexData[2].ulCount >= vVertexData[3].ulCount) {
                    pvMaxVertex = &vVertexData[2];
                    ulTemp = vVertexData[2].ulCount;
                } else {
                    pvMaxVertex = &vVertexData[3];
                    ulTemp = vVertexData[3].ulCount;
                }
            }

            if (vVertexData[1].ulCount > ulTemp) {
                pvMaxVertex = &vVertexData[1];
                ulTemp = vVertexData[1].ulCount;
            }
            if (vVertexData[0].ulCount > ulTemp) {
                pvMaxVertex = &vVertexData[0];
            }

            pvVertexDataEnd = pvVertexData;

            /* Prepare a dword version of the most common vertex number (color) */
            ulColor = ulNibbleToDword[pvMaxVertex->ulVertex];

            /* Mark that the vertex we're about to do doesn't need to be done */
            /* later */
            pvMaxVertex->ulVertex = 0xFF;

            /* Block fill the dither pattern with the more common vertex */
            pulTemp = (ULONG *)ajDither;
            *pulTemp = ulColor;
            *(pulTemp+1) = ulColor;
            *(pulTemp+2) = ulColor;
            *(pulTemp+3) = ulColor;
            *(pulTemp+4) = ulColor;
            *(pulTemp+5) = ulColor;
            *(pulTemp+6) = ulColor;
            *(pulTemp+7) = ulColor;
            *(pulTemp+8) = ulColor;
            *(pulTemp+9) = ulColor;
            *(pulTemp+10) = ulColor;
            *(pulTemp+11) = ulColor;
            *(pulTemp+12) = ulColor;
            *(pulTemp+13) = ulColor;
            *(pulTemp+14) = ulColor;
            *(pulTemp+15) = ulColor;

            /* Now dither all the remaining vertices in order 0->2 or 0->3 */
            /* (in order of increasing intensity) */
            pulDitherOrder = aulDitherOrder;
            pvVertexData = vVertexData;
            do {
                if (pvVertexData->ulVertex == 0xFF) {
                    /* This is the max vertex, which we already did, but we */
                    /* have to account for it in the dither order */
                    pulDitherOrder += pvVertexData->ulCount;
                } else {
                    jColor = (BYTE) pvVertexData->ulVertex;
                    ulNumPixels = pvVertexData->ulCount;
                    switch (ulNumPixels & 3) {
                        case 3:
                            ajDither[*(pulDitherOrder+2)] = jColor;
                        case 2:
                            ajDither[*(pulDitherOrder+1)] = jColor;
                        case 1:
                            ajDither[*(pulDitherOrder+0)] = jColor;
                            pulDitherOrder += ulNumPixels & 3;
                        case 0:
                            break;
                    }
                    if ((ulNumPixels >>= 2) != 0) {
                        do {
                            ajDither[*pulDitherOrder] = jColor;
                            ajDither[*(pulDitherOrder+1)] = jColor;
                            ajDither[*(pulDitherOrder+2)] = jColor;
                            ajDither[*(pulDitherOrder+3)] = jColor;
                            pulDitherOrder += sizeof(ULONG);
                        } while (--ulNumPixels);
                    }
                }
            } while (++pvVertexData < pvVertexDataEnd);

        } else if (ulTemp == 2) {

            /* There are exactly two vertices with more than zero pixels; fill
             in the dither array as follows: block fill with vertex with more
             points first, then dither in the other vertex */
            if (vVertexData[0].ulCount >= vVertexData[1].ulCount) {
                /* There are no more vertex 1 than vertex 0 pixels, so do */
                /* the block fill with vertex 0 */
                ulColor = ulNibbleToDword[vVertexData[0].ulVertex];
                /* Do the dither with vertex 1 */
                jColor = (BYTE) vVertexData[1].ulVertex;
                ulNumPixels = vVertexData[1].ulCount;
                /* Set where to start dithering with vertex 1 (vertex 0 is */
                /* lower intensity, so its pixels come first in the dither */
                /* order) */
                pulDitherOrder = aulDitherOrder + vVertexData[0].ulCount;
            } else {
                /* There are more vertex 1 pixels, so do the block fill */
                /* with vertex 1 */
                ulColor = ulNibbleToDword[vVertexData[1].ulVertex];
                /* Do the dither with vertex 0 */
                jColor = (BYTE) vVertexData[0].ulVertex;
                ulNumPixels = vVertexData[0].ulCount;
                /* Set where to start dithering with vertex 0 (vertex 0 is */
                /* lower intensity, so its pixels come first in the dither */
                /* order) */
                pulDitherOrder = aulDitherOrder;
            }

            /* Block fill the dither pattern with the more common vertex */
            pulTemp = (ULONG *)ajDither;
            *pulTemp = ulColor;
            *(pulTemp+1) = ulColor;
            *(pulTemp+2) = ulColor;
            *(pulTemp+3) = ulColor;
            *(pulTemp+4) = ulColor;
            *(pulTemp+5) = ulColor;
            *(pulTemp+6) = ulColor;
            *(pulTemp+7) = ulColor;
            *(pulTemp+8) = ulColor;
            *(pulTemp+9) = ulColor;
            *(pulTemp+10) = ulColor;
            *(pulTemp+11) = ulColor;
            *(pulTemp+12) = ulColor;
            *(pulTemp+13) = ulColor;
            *(pulTemp+14) = ulColor;
            *(pulTemp+15) = ulColor;

            /* Dither in the less common vertex */
            switch (ulNumPixels & 3) {
                case 3:
                    ajDither[*(pulDitherOrder+2)] = jColor;
                case 2:
                    ajDither[*(pulDitherOrder+1)] = jColor;
                case 1:
                    ajDither[*(pulDitherOrder+0)] = jColor;
                    pulDitherOrder += ulNumPixels & 3;
                case 0:
                    break;
            }
            if ((ulNumPixels >>= 2) != 0) {
                do {
                    ajDither[*pulDitherOrder] = jColor;
                    ajDither[*(pulDitherOrder+1)] = jColor;
                    ajDither[*(pulDitherOrder+2)] = jColor;
                    ajDither[*(pulDitherOrder+3)] = jColor;
                    pulDitherOrder += sizeof(ULONG);
                } while (--ulNumPixels);
            }

        } else {

            /* There is only one vertex in this dither */

            /* No sorting or dithering is needed for just one color; we can */
            /* just generate the final DIB directly */
            ulColor = ulNibbleToDword[vVertexData[0].ulVertex];
            ulColor |= ulColor << 4;
            *pul = ulColor;
            *(pul+1) = ulColor;
            *(pul+2) = ulColor;
            *(pul+3) = ulColor;
            *(pul+4) = ulColor;
            *(pul+5) = ulColor;
            *(pul+6) = ulColor;
            *(pul+7) = ulColor;
            return 1;
        }

        /* Now convert the 64 bytes into the 4BPP Engine Format Bitmap */
        pulTemp = (ULONG *)ajDither;

        *pul = (*pulTemp << 4) | *(pulTemp + 1);
        *(pul + 1) = (*(pulTemp + 2) << 4) | *(pulTemp + 3);
        *(pul + 2) = (*(pulTemp + 4) << 4) | *(pulTemp + 5);
        *(pul + 3) = (*(pulTemp + 6) << 4) | *(pulTemp + 7);
        *(pul + 4) = (*(pulTemp + 8) << 4) | *(pulTemp + 9);
        *(pul + 5) = (*(pulTemp + 10) << 4) | *(pulTemp + 11);
        *(pul + 6) = (*(pulTemp + 12) << 4) | *(pulTemp + 13);
        *(pul + 7) = (*(pulTemp + 14) << 4) | *(pulTemp + 15);

    } else {

        /* For monochrome we will only use the Intensity (grey level) */
        memset((LPVOID) pul, 0, PATTERNSIZE/2);  /* zero the dither bits */

        ulRed   = (ULONG) ((PALETTEENTRY *) &rgb)->peRed;
        ulGre = (ULONG) ((PALETTEENTRY *) &rgb)->peGreen;
        ulBlu  = (ULONG) ((PALETTEENTRY *) &rgb)->peBlue;

        /* I = .30R + .59G + .11B
         For convience the following ratios are used:
        
          77/256 = 30.08%
         151/256 = 58.98%
          28/256 = 10.94% */

        ulGrey  = (((ulRed * 77) + (ulGre * 151) + (ulBlu * 28)) >> 8) & 255;

        /* Convert the RGBI from 0-255 to 0-64 notation. */

        ulGrey = (ulGrey + 1) >> 2;

        while(ulGrey) {
            ulGrey--;
            pul[ajByte[ulGrey]] |= ((ULONG) ajBits[ulGrey]);
        }
	for (i = 0; i < PATTERNSIZE / (2 * sizeof(ULONG)); i++) {
	    ulVertex0Temp = GETDWORD(&pul[i]);
	    pul[i] = ulVertex0Temp;
	}

    }

    return 1;
}

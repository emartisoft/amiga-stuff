#include <exec/exec.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <graphics/gfx.h>
#include <hardware/custom.h>
#include <hardware/dmabits.h>

#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/graphics_protos.h>
#include <clib/intuition_protos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hardware.h"
#include "cop_all.h"
#include "global_defs.h"
#include "common.h"

#define EXTRAHEIGHT 32

#define BITMAPWIDTH SCREENWIDTH
#define BITMAPBYTESPERROW (BITMAPWIDTH / 8)
#define BITMAPHEIGHT ((SCREENHEIGHT + EXTRAHEIGHT) * 2)
#define HALFBITMAPHEIGHT (BITMAPHEIGHT / 2)

#define BLOCKSWIDTH 320
#define BLOCKSHEIGHT 200

#define BLOCKSBYTESPERROW (BLOCKSWIDTH / 8)
#define BLOCKSPERROW (BLOCKSWIDTH / BLOCKWIDTH)

#define BITMAPBLOCKSPERROW (BITMAPWIDTH / BLOCKWIDTH)
#define BITMAPBLOCKSPERCOL (BITMAPHEIGHT / BLOCKHEIGHT)
#define HALFBITMAPBLOCKSPERCOL (BITMAPBLOCKSPERCOL / 2)

#define VISIBLEBLOCKSX (SCREENWIDTH / BLOCKWIDTH)
#define VISIBLEBLOCKSY (SCREENHEIGHT / BLOCKHEIGHT)

#define BITMAPPLANELINES (BITMAPHEIGHT * BLOCKSDEPTH)
#define BLOCKPLANELINES  (BLOCKHEIGHT * BLOCKSDEPTH)

#define BLOCKSFILESIZE (BLOCKSWIDTH * BLOCKSHEIGHT * BLOCKSPLANES / 8 + PALSIZE)

// calculate how many times (steps) y-scrolling needs to
// blit two blocks instead of one block to make sure a
// complete row is blitted after 16 pixels of y-scrolling
//
// x * 2 + (16 - x) = BITMAPBLOCKSPERROW
// 2x + 16 - x = BITMAPBLOCKSPERROW
// x = BITMAPBLOCKSPERROW - 16

#define TWOBLOCKS (BITMAPBLOCKSPERROW - NUMSTEPS)
#define TWOBLOCKSTEP (NUMSTEPS - TWOBLOCKS)

struct Screen *scr;
struct RastPort *ScreenRastPort;
struct BitMap *BlocksBitmap,*ScreenBitmap;
UBYTE	 *frontbuffer, *blocksbuffer;

WORD	mapposy,videoposy;
WORD	bitmapheight;

struct PrgOptions options;
struct LevelMap level_map;

UWORD	colors[BLOCKSCOLORS];
char	s[256];

struct FetchInfo fetchinfo [] =
{
	{0x38,0xD0,0},	/* normal         */
	{0x38,0xC8,0}, /* BPL32          */
	{0x38,0xC8,0}, /* BPAGEM         */
	{0x38,0xB8,0}	/* BPL32 + BPAGEM */
};

/************* SETUP/CLEANUP ROUTINES ***************/

static void Cleanup (char *msg)
{
	WORD rc;
	if (msg) {
		printf("Error: %s\n",msg);
		rc = RETURN_WARN;
	} else {
		rc = RETURN_OK;
	}

	if (scr) CloseScreen(scr);

	if (ScreenBitmap) {
		WaitBlit();
		FreeBitMap(ScreenBitmap);
	}

	if (BlocksBitmap) {
		WaitBlit();
		FreeBitMap(BlocksBitmap);
	}
	if (level_map.raw_map) FreeVec(level_map.raw_map);
	exit(rc);
}

static void OpenDisplay(void)
{
	DisplayInfoHandle		dih;
	struct DimensionInfo diminfo;
	ULONG						modeid;
	LONG						l;

	bitmapheight = BITMAPHEIGHT + 3;

	if (!(ScreenBitmap = AllocBitMap(BITMAPWIDTH, bitmapheight, BLOCKSDEPTH,
                                     BMF_STANDARD | BMF_INTERLEAVED | BMF_CLEAR,0))) {
		Cleanup("Can't alloc screen bitmap!");
	}
	frontbuffer = ScreenBitmap->Planes[0];

	if (!(TypeOfMem(ScreenBitmap->Planes[0]) & MEMF_CHIP)) {
		Cleanup("Screen bitmap is not in CHIP RAM!?? If you have a gfx card try disabling \"planes to fast\" or similiar options in your RTG system!");
	}

	l = GetBitMapAttr(ScreenBitmap,BMA_FLAGS);
	if (!(GetBitMapAttr(ScreenBitmap,BMA_FLAGS) & BMF_INTERLEAVED))	{
		Cleanup("Screen bitmap is not in interleaved format!??");
	}

    modeid = get_mode_id(options.how, options.ntsc);
	if (!(scr = OpenScreenTags(0,SA_Width,BITMAPWIDTH,
										  SA_Height,bitmapheight,
										  SA_Depth,BLOCKSDEPTH,
										  SA_DisplayID,modeid,
										  SA_BitMap,ScreenBitmap,
										  options.how ? SA_Overscan : TAG_IGNORE,OSCAN_TEXT,
										  options.how ? SA_AutoScroll : TAG_IGNORE,TRUE,
										  SA_Quiet,TRUE,
										  TAG_DONE)))
	{
		Cleanup("Can't open screen!");
	}

	if (scr->RastPort.BitMap->Planes[0] != ScreenBitmap->Planes[0])
	{
		Cleanup("Screen was not created with the custom bitmap I supplied!??");
	}
	ScreenRastPort = &scr->RastPort;
	LoadRGB4(&scr->ViewPort,colors,BLOCKSCOLORS);
}

static void InitCopperlist(void)
{
	WORD	*wp;
	LONG l;

	wait_vbl();

	custom->dmacon = 0x7FFF;
	custom->beamcon0 = options.ntsc ? 0 : DISPLAYPAL;

	CopFETCHMODE[1] = options.fetchmode;

	// bitplane control registers

	CopBPLCON0[1] = ((BLOCKSDEPTH * BPL0_BPU0_F) & BPL0_BPUMASK) +
						 ((BLOCKSDEPTH / 8) * BPL0_BPU3_F) +
						 BPL0_COLOR_F +
						 (options.speed ? 0 : BPL0_USEBPLCON3_F);

	CopBPLCON1[1] = 0;

	CopBPLCON3[1] = BPLCON3_BRDNBLNK;

	// bitplane modulos

	l = BITMAPBYTESPERROW * BLOCKSDEPTH -
		 SCREENBYTESPERROW - fetchinfo[options.fetchmode].modulooffset;

	CopBPLMODA[1] = l;
	CopBPLMODB[1] = l;

	// display window start/stop
	CopDIWSTART[1] = DIWSTART;
	CopDIWSTOP[1] = DIWSTOP;

	// display data fetch start/stop
	CopDDFSTART[1] = fetchinfo[options.fetchmode].ddfstart;
	CopDDFSTOP[1]  = fetchinfo[options.fetchmode].ddfstop;

	// plane pointers

	wp = CopPLANE1H;

	for (l = 0; l < BLOCKSDEPTH; l++) {
		wp[1] = (WORD)(((ULONG)ScreenBitmap->Planes[l]) >> 16);
		wp[3] = (WORD)(((ULONG)ScreenBitmap->Planes[l]) & 0xFFFF);
		wp += 4;
	}
	custom->intena = 0x7FFF;
	custom->dmacon = DMAF_SETCLR | DMAF_BLITTER | DMAF_COPPER | DMAF_RASTER | DMAF_MASTER;
	custom->cop2lc = (ULONG)CopperList;
}

/******************* SCROLLING **********************/

static void DrawBlock(LONG x,LONG y,LONG mapx,LONG mapy)
{
	UBYTE block;

	// x = in pixels
	// y = in "planelines" (1 realline = BLOCKSDEPTH planelines)
	x = (x / 8) & 0xFFFE;
	y = y * BITMAPBYTESPERROW;

	block = level_map.data[mapy * level_map.width + mapx];

	mapx = (block % BLOCKSPERROW) * (BLOCKWIDTH / 8);
	mapy = (block / BLOCKSPERROW) * (BLOCKPLANELINES * BLOCKSBYTESPERROW);

	if (options.how) OwnBlitter();

	hard_wait_blit();

	custom->bltcon0 = 0x9F0;	// use A and D. Op: D = A
	custom->bltcon1 = 0;
	custom->bltafwm = 0xFFFF;
	custom->bltalwm = 0xFFFF;
	custom->bltamod = BLOCKSBYTESPERROW - (BLOCKWIDTH / 8);
	custom->bltdmod = BITMAPBYTESPERROW - (BLOCKWIDTH / 8);
	custom->bltapt  = blocksbuffer + mapy + mapx;
	custom->bltdpt	 = frontbuffer + y + x;

	custom->bltsize = BLOCKPLANELINES * 64 + (BLOCKWIDTH / 16);

	if (options.how) DisownBlitter();
}

static void FillScreen(void)
{
	WORD a,b,x,y;

	for (b = 0; b < HALFBITMAPBLOCKSPERCOL; b++) {
		for (a = 0; a < BITMAPBLOCKSPERROW; a++) {
			x = a * BLOCKWIDTH;
			y = b * BLOCKPLANELINES;
			DrawBlock(x,y,a,b);
			DrawBlock(x,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,a,b);
		}
	}
}

static void ScrollUp(void)
{
	WORD mapx,mapy,x,y;

	if (mapposy < 1) return;

	mapposy--;
	videoposy = mapposy % HALFBITMAPHEIGHT;

	mapx = mapposy & (NUMSTEPS - 1);
	mapy = mapposy / BLOCKHEIGHT;
	y = ROUND2BLOCKHEIGHT(videoposy) * BLOCKSDEPTH;

    if (mapx < TWOBLOCKSTEP) {
        // blit only one block per half bitmap
        x = mapx * BLOCKWIDTH;
        DrawBlock(x,y,mapx,mapy);
        DrawBlock(x,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx,mapy);
    } else {
        // blit two blocks per half bitmap
        mapx = TWOBLOCKSTEP + (mapx - TWOBLOCKSTEP) * 2;
        x = mapx * BLOCKWIDTH;

        DrawBlock(x,y,mapx,mapy);
        DrawBlock(x,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx,mapy);

        DrawBlock(x + BLOCKWIDTH,y,mapx + 1,mapy);
        DrawBlock(x + BLOCKWIDTH,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx + 1,mapy);
    }
}

static void ScrollDown(void)
{
	WORD mapx,mapy,x,y;

	if (mapposy >= (level_map.height * BLOCKHEIGHT - SCREENHEIGHT - BLOCKHEIGHT)) return;

	mapx = mapposy & (NUMSTEPS - 1);
	mapy = HALFBITMAPBLOCKSPERCOL + mapposy / BLOCKHEIGHT;

	y = ROUND2BLOCKHEIGHT(videoposy) * BLOCKSDEPTH;

    if (mapx < TWOBLOCKSTEP) {
        // blit only one block per half bitmap
        x = mapx * BLOCKWIDTH;
        DrawBlock(x,y,mapx,mapy);
        DrawBlock(x,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx,mapy);
    } else {
        // blit two blocks per half bitmap
        mapx = TWOBLOCKSTEP + (mapx - TWOBLOCKSTEP) * 2;
        x = mapx * BLOCKWIDTH;

        DrawBlock(x,y,mapx,mapy);
        DrawBlock(x,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx,mapy);

        DrawBlock(x + BLOCKWIDTH,y,mapx + 1,mapy);
        DrawBlock(x + BLOCKWIDTH,y + HALFBITMAPHEIGHT * BLOCKSDEPTH,mapx + 1,mapy);
    }

	mapposy++;
	videoposy = mapposy % HALFBITMAPHEIGHT;
}

static void CheckJoyScroll(void)
{
	WORD i,count;

	if (joy_fire()) count = 8; else count = 1;

	if (joy_up()) {
		for (i = 0; i < count; i++)	{
			ScrollUp();
		}
	}

	if (joy_down()) {
		for (i = 0; i < count; i++) {
			ScrollDown();
		}
	}
}

static void UpdateCopperlist(void)
{
	ULONG pl;
	LONG	planeadd;
	WORD i;
	WORD *wp;

	planeadd = ((LONG)(videoposy + BLOCKHEIGHT)) * BITMAPBYTESPERROW * BLOCKSDEPTH;

	// set plane pointers
	wp = CopPLANE1H;

	for (i = 0; i < BLOCKSDEPTH; i++) {
		pl = ((ULONG)ScreenBitmap->Planes[i]) + planeadd;
		wp[1] = (WORD)(pl >> 16);
		wp[3] = (WORD)(pl & 0xFFFF);
		wp += 4;
	}
}

static void MainLoop(void)
{
	if (!options.how) {
		// activate copperlist
		hard_wait_blit();
		wait_vbl();
		custom->copjmp2 = 0;
	}

	while (!lmb_down()) {
		if (!options.how) {
			wait_vbeam(199);
			wait_vbeam(200);
		} else {
			Delay(1);
		}
		if (options.speed) *(WORD *)0xdff180 = 0xFF0;

		CheckJoyScroll();

		if (options.speed) *(WORD *)0xdff180 = 0xF00;

		if (!options.how) {
			UpdateCopperlist();
		}
	}
}

/********************* MAIN *************************/

int main(int argc, char **argv)
{
	BOOL res = get_arguments(&options, s);
    if (!res) Cleanup(s);
	res = read_level_map(RACE_MAP_PATH, &level_map, s);
    if (!res) Cleanup(s);

	BlocksBitmap = read_blocks(RACE_BLOCKS_PATH, colors, s, BLOCKSWIDTH, BLOCKSHEIGHT);
    if (!BlocksBitmap) Cleanup(s);
	blocksbuffer = BlocksBitmap->Planes[0];

	OpenDisplay();

	if (!options.how) {
		Delay(2*50);
		kill_system();
		InitCopperlist();
	}
	FillScreen();
	MainLoop();

	if (!options.how) {
		activate_system();
	}
	Cleanup(0);
    return 0;
}

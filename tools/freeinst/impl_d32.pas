{**************************************
 *  System-dependent implementation   *
 *  of low-level functions for DPMI32 *
 **************************************}

unit Impl_D32;

interface


procedure Open_Disk(Drive: PChar; var DevHandle: Hfile);
procedure Read_Disk(devhandle: Hfile; var buf; buf_len: Ulong);
procedure Write_Disk(devhandle: Hfile; var buf; buf_len: Ulong);
procedure Close_Disk(DevHandle: Hfile);
procedure Lock_Disk(DevHandle: Hfile);
procedure Unlock_Disk(DevHandle: Hfile);

procedure Read_MBR_Sector(DriveNum: char; var MBRBuffer);
procedure Write_MBR_Sector(DriveNum: char; var MBRBuffer);
procedure Backup_MBR_Sector;
procedure Restore_MBR_Sector;

//procedure Fat32FSctrl(DevHandle: Hfile);
//procedure Fat32WriteSector(DevHandle: hfile; ulSector: ULONG; nSectors: USHORT; var buf);

implementation

uses
  System;

type
  PRModeCall = ^TRModeCall;
  TRModeCall = record
    edi, esi, ebp, reserved, ebx, edx, ecx, eax: ULong;
    flags, es, ds, fs, gs, ip, cs, sp, ss: UShort;
  end;

  PSREGS = ^TSREGS;
  TSREGS = packed record
    es, cs, ss, ds, fs, gs: Word;
  end;

  PREGS  = ^TREGS;
  TREGS  = packed record
    case n of
      0:
        ax, bx, cx, dx: Word;
      1:
        al, ah, bl, bh, cl, ch, dl, dh: Byte;
      si, di: Word;
  end;

  CMDS = (READ_CMD, WRITE_CMD, PARA_CMD);

{*
 *  Performs a real mode interrupt from protected mode
 *  routines dpmi_rmode_intr and int86x are 'stolen' from A.Schulman's
 *  Undocumented DOS
 *}
function dpmi_rmode_intr(intno, flags, copywords: Word, rmc: PRModeCall): boolean; assembler; {$ASMMODE intel}
asm
  push di
  push bx
  push cx
  mov ax, 0300h             // simulate real mode interrupt
  mov bx, intno             // interrupt number, flags
  mov cx, copywords         // words to copy from pmode to rmode stack
  lea di, rmc               // ES:DI = address of rmode call struct
  int 31h                   // call DPMI
  jc error
  mov ax, 1                 // return TRUE
  jmp short done
@error:
  mov ax, 0                 // return FALSE
@done:
  pop cx
  pop bx
  pop di
end;

function int86x(intno: integer; inregs, outregs: PREGS, sregs: PSREGS): integer;
var
  r: TRModeCall;
begin
  FillChar(@r, sizeof(r), 0);   { initialize all fields to zero: important! }
  r.edi := inregs^.di;
  r.esi := inregs^.si;
  r.ebx := inregs^.bx;
  r.edx := inregs^.dx;
  r.ecx := inregs^.cx;
  r.eax := inregs^.ax;
  r.flags := inregs^.cflag;
  r.es := sregs^.es;
  r.ds := sregs^.ds;
  r.cs := sregs^.cs;

  if not dpmi_rmode_intr(intno, 0, 0, @r) then
  begin
    outregs^.cflag := 1;          { error: set carry flag! }
    result := -1;
    exit;
  end

  sregs^.es := r.es;
  sregs^.cs := r.cs;
  sregs^.ss := r.ss;
  sregs^.ds := r.ds;
  outregs^.ax := r.eax;
  outregs^.bx := r.ebx;
  outregs^.cx := r.ecx;
  outregs^.dx := r.edx;
  outregs^.si := r.esi;
  outregs^.di := r.edi;
  outregs^.cflag := r.flags and 1;  { carry flag }

  result := outregs^.ax;
end;

function biosdisk(cmd: CMDS; drive, head, cyl, sector, nsects: LongInt; var buffer): integer;
  lparam: Pointer;
  rmSegment, pmSelektor: LongInt;
  regs: TRegs;
  sregs: TSRegs;
begin
    if cmd = READ_CMD then
    begin
        { allocate DOS-Memory }
        GetMem(lparam, nsects);
        if lparam = 0 then
        begin
            Writeln("cannot allocate DOS memory");
            exit(-1);
        end;
        //rmSegment  := HIWORD(lparam);
        //pmSelektor := LOWORD(lparam);
        sregs.cs := 0;
        sregs.ds := 0;
        sregs.es := rmSegment;
        regs.bx  := 0;
        regs.dl  := drive;
        regs.dh  := head;
        regs.cl  := (sector & 0x3F) + ((cyl >> 2) & 0xC0);
        regs.ch  := cyl;
        regs.al  := nsects;
        regs.ah  := 0x02;
        int86x($13, @regs, @regs, @sregs);                /*Bios Disk Read Interrupt */
        if (regs.x.flags)
        {
            GlobalDosFree(pmSelektor);                          /*free DOS-Memory */
            return -1;                                          /*Fehler */
        }
        _fmemcpy(buffer, MK_FP(pmSelektor, 0), DISK_BLOCK_SIZE * nsects);
        GlobalDosFree(pmSelektor);                              /*free DOS-Memory */
        return (regs.x.ax) & 0xFF00;
    end else if cmd = PARA_CMD then
    begin

        DebugOut(2,"---PARA_CMD----\n");

        regs.h.ah = 0x08;
        regs.h.dl = drive;
        real_int86x(0x13, &regs, &regs, &sregs);                /*Bios Disk Read Interrupt */
        if (regs.x.flags)
        {
            return -1;                                          /*Fehler */
        }
        buffer[0] = regs.h.cl;                                  /*no of sectors */
        buffer[1] = regs.h.ch;                                  /*no of cylinders */
        buffer[2] = regs.h.dl;                                  /*no of drives */
        buffer[3] = regs.h.dh;                                  /*no of heads */
        return (regs.x.ax) & 0xFF00;
    } else if (cmd == WRITE_CMD)
    {
        fprintf(STDERR,"Sorry, lwrite currently not supported under Windows GUI. Use DOS box\n");
    } else
        fprintf(STDERR,"Illegal command in function biosdisk()\n");

    return -1;
}

procedure Read_Sectors(Drive: char; var Buf; StartSec, Sectors: ULong);
begin

end;

{$IFDEF FPC}
{initialisation}
begin
{$ENDIF}
end.

/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "fileOpsCommon.h"
#include <array>
#include <vector>
#include <math.h>

#define VIC_CLOCK_NTSC (315000000.0/88.0*2.0/7.0)
#define VIC_CLOCK_PAL  ((283.75*15625.0+25.0)/4.0)

// per-channel divisor to invert the dispatch's right-shift (ch0>>=2, ch1>>=1, ch2>>=0, noise>>=1)
static const int VIC_CHMUL[4]={4,2,1,2};
#define VIC_CHIP_DIVIDER 32

static double vicRegToFreq(uint8_t reg, int ch, double chipClock) {
  if (!(reg&0x80)) return 0.0;
  int a=(255-reg)&127;
  if (a==0) a=128;
  return chipClock/((double)(VIC_CHIP_DIVIDER*a*VIC_CHMUL[ch]));
}

static int freqToNote(double freq) {
  if (freq<=0.0) return -1;
  double n=12.0*log2(freq/27.5)+57.0;
  return (int)round(n);
}

bool DivEngine::loadVIC(unsigned char* file, size_t len) {
  struct InvalidHeaderException {};
  SafeReader reader=SafeReader(file,len);
  warnings="";

  try {
    char magic[4];
    reader.read(magic,4);
    if (memcmp(magic,"VIC\0",4)!=0) {
      logD("not a VIC file");
      throw EndOfFileException(&reader,reader.tell());
    }

    uint8_t version=reader.readC();
    (void)version;

    char titleBuf[33]; memset(titleBuf,0,33);
    reader.read(titleBuf,32);

    char authorBuf[33]; memset(authorBuf,0,33);
    reader.read(authorBuf,32);

    float tickRate=reader.readF();
    uint32_t loopFrame=(uint32_t)reader.readI();
    uint32_t frameCount=(uint32_t)reader.readI();

    uint8_t clockSel=0;
    if (len>81) clockSel=file[81];

    double chipClock=clockSel?VIC_CLOCK_PAL:VIC_CLOCK_NTSC;

    if (tickRate>200.0f) tickRate=clockSel?50.0f:60.0f;

    reader.seek(84,SEEK_SET);

    std::vector<std::array<uint8_t,5>> frames;

    if (frameCount>0) {
      for (uint32_t i=0; i<frameCount; i++) {
        if ((size_t)reader.tell()+5>(size_t)len) break;
        std::array<uint8_t,5> f;
        reader.read(f.data(),5);
        frames.push_back(f);
      }
    } else {
      reader.seek(80,SEEK_SET);
      while ((size_t)reader.tell()+5<=(size_t)len) {
        std::array<uint8_t,5> f;
        reader.read(f.data(),5);
        if (f[0]==0&&f[1]==0&&f[2]==0&&f[3]==0&&f[4]==0) break;
        frames.push_back(f);
      }
    }

    if (frames.empty()) {
      lastError="VIC file has no frame data";
      delete[] file;
      return false;
    }

    DivSong ds;
    ds.name=String(titleBuf);
    ds.author=String(authorBuf);
    ds.version=DIV_VERSION_MOD;
    ds.systemLen=1;
    ds.system[0]=DIV_SYSTEM_VIC20;
    ds.systemFlags[0].set("clockSel",(int)clockSel);
    ds.subsong[0]->hz=tickRate;
    ds.subsong[0]->speeds.val[0]=1;
    ds.subsong[0]->speeds.len=1;

    for (int ch=0; ch<4; ch++) {
      ds.subsong[0]->chanShow[ch]=true;
      ds.subsong[0]->chanShowChanOsc[ch]=true;
    }
    ds.subsong[0]->chanName[0]="CH1";
    ds.subsong[0]->chanName[1]="CH2";
    ds.subsong[0]->chanName[2]="CH3";
    ds.subsong[0]->chanName[3]="Noise";
    ds.subsong[0]->chanShortName[0]="1";
    ds.subsong[0]->chanShortName[1]="2";
    ds.subsong[0]->chanShortName[2]="3";
    ds.subsong[0]->chanShortName[3]="N";

    for (int ch=0; ch<4; ch++) {
      DivInstrument* ins=new DivInstrument;
      ins->type=DIV_INS_VIC;
      switch (ch) {
        case 0: ins->name="CH1"; break;
        case 1: ins->name="CH2"; break;
        case 2: ins->name="CH3"; break;
        case 3: ins->name="Noise"; break;
      }
      ds.ins.push_back(ins);
    }
    ds.insLen=(int)ds.ins.size();

    int totalFrames=(int)frames.size();
    int patLen=128;
    int numOrders=(totalFrames+patLen-1)/patLen;
    if (numOrders<1) numOrders=1;
    if (numOrders>DIV_MAX_PATTERNS) numOrders=DIV_MAX_PATTERNS;

    ds.subsong[0]->ordersLen=numOrders;
    ds.subsong[0]->patLen=patLen;

    for (int o=0; o<numOrders; o++) {
      for (int ch=0; ch<4; ch++) {
        ds.subsong[0]->orders.ord[ch][o]=(unsigned char)o;
      }
    }

    uint8_t prevReg[4]={0,0,0,0};
    uint8_t prevVol=0;
    bool prevActive[4]={false,false,false,false};

    for (int fi=0; fi<totalFrames; fi++) {
      int ord=fi/patLen;
      int row=fi%patLen;
      if (ord>=numOrders) break;

      const auto& f=frames[fi];
      uint8_t vol=f[4]&0x0F;

      for (int ch=0; ch<4; ch++) {
        uint8_t reg=f[ch];
        bool active=(reg&0x80)!=0;

        DivPattern* pat=ds.subsong[0]->pat[ch].getPattern(ord,true);
        short* row_data=pat->newData[row];

        row_data[DIV_PAT_NOTE]=-1;
        row_data[DIV_PAT_INS]=-1;
        row_data[DIV_PAT_VOL]=-1;

        if (active && !prevActive[ch]) {
          double freq=vicRegToFreq(reg,ch,chipClock);
          int note=freqToNote(freq);
          if (note>=0 && note<200) {
            row_data[DIV_PAT_NOTE]=(short)note;
            row_data[DIV_PAT_INS]=(short)ch;
            row_data[DIV_PAT_VOL]=(short)vol;
          }
        } else if (!active && prevActive[ch]) {
          row_data[DIV_PAT_NOTE]=DIV_NOTE_OFF;
        } else if (active && reg!=prevReg[ch]) {
          double freq=vicRegToFreq(reg,ch,chipClock);
          int note=freqToNote(freq);
          if (note>=0 && note<200) {
            row_data[DIV_PAT_NOTE]=(short)note;
            row_data[DIV_PAT_INS]=(short)ch;
          }
        }

        if (ch==0 && vol!=prevVol && active) {
          row_data[DIV_PAT_VOL]=(short)vol;
        }

        prevReg[ch]=reg;
        prevActive[ch]=active;
      }
      prevVol=vol;
    }

    if (loopFrame!=0xFFFFFFFF && loopFrame<(uint32_t)totalFrames) {
      int loopOrd=(int)loopFrame/patLen;
      int lastFi=totalFrames-1;
      int lastOrd=lastFi/patLen;
      int lastRow=lastFi%patLen;
      DivPattern* pat=ds.subsong[0]->pat[0].getPattern(lastOrd,true);
      pat->newData[lastRow][DIV_PAT_FX(0)]=0x0B;
      pat->newData[lastRow][DIV_PAT_FX(0)+1]=(short)loopOrd;
    }

    ds.subsong[0]->name="";

    ds.initDefaultSystemChans();
    ds.recalcChans();
    ds.findSubSongs();

    if (active) quitDispatch();
    BUSY_BEGIN_SOFT;
    saveLock.lock();
    song.unload();
    song=ds;
    hasLoadedSomething=true;
    changeSong(0);
    saveLock.unlock();
    BUSY_END;
    if (active) {
      initDispatch();
      BUSY_BEGIN;
      renderSamples();
      reset();
      BUSY_END;
    }

    logI("loaded VIC file: %s by %s, %d frames at %.1fHz",
         titleBuf,authorBuf,totalFrames,(double)tickRate);
    delete[] file;
    return true;

  } catch (EndOfFileException& e) {
    logE("premature end of file!");
    lastError="incomplete file";
  } catch (InvalidHeaderException&) {
    logE("invalid header!");
    lastError="invalid file";
  }
  delete[] file;
  return false;
}

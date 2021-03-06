/*
    Copyright (C) 2014 CSP Innovazione nelle ICT s.c.a r.l. (http://www.csp.it/)
    Copyright (C) 2014 Matthias P. Braendli (http://www.opendigitalradio.org)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    etisnoop.cpp
          Parse ETI NI G.703 file

    Authors:
         Sergio Sagliocco <sergio.sagliocco@csp.it>
         Matthias P. Braendli <matthias@mpb.li>
*/



#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include "lib_crc.h"

#include "dabplussnoop.h"
#include "etiinput.h"

struct FIG
{
    int type;
    int ext;
    int len;
};

class FIGalyser
{
    public:
        FIGalyser()
        {
            clear();
        }

        void set_fib(int fib)
        {
            m_fib = fib;
        }

        void push_back(int type, int ext, int len)
        {
            struct FIG fig = {
                .type = type,
                .ext  = ext,
                .len  = len };

            m_figs[m_fib].push_back(fig);
        }

        void analyse()
        {
            printf("FIC ");

            for (size_t fib = 0; fib < m_figs.size(); fib++) {
                int consumed = 7;
                int fic_size = 0;
                printf("[%1d ", fib);

                for (size_t i = 0; i < m_figs[fib].size(); i++) {
                    FIG &f = m_figs[fib][i];
                    printf("%01d/%02d (%2d) ", f.type, f.ext, f.len);

                    consumed += 10;

                    fic_size += f.len;
                }

                printf(" ");

                int align = 60 - consumed;
                if (align > 0) {
                    while (align--) {
                        printf(" ");
                    }
                }

                printf("|");

                for (int i = 0; i < 15; i++) {
                    if (2*i < fic_size) {
                        printf("#");
                    }
                    else {
                        printf("-");
                    }
                }

                printf("| ]   ");

            }

            printf("\n");
        }

        void clear()
        {
            m_figs.clear();
            m_figs.resize(3);
        }

    private:
        int m_fib;
        std::vector<std::vector<FIG> > m_figs;
};


struct FIG0_13_shortAppInfo
{
    uint16_t SId;
    uint8_t No:4;
    uint8_t SCIdS:4;
} PACKED;


#define ETINIPACKETSIZE 6144

using namespace std;

struct eti_analyse_config_t {
    FILE* etifd;
    bool ignore_error;
    std::map<int, DabPlusSnoop> streams_to_decode;
    bool analyse_fic_carousel;
};

// Globals
static int verbosity;

// Function prototypes
void printinfo(string header,
        int indent_level,
        int min_verb=0);

void printbuf(string header,
        int indent_level,
        unsigned char* buffer,
        size_t size,
        string desc="");

void decodeFIG(FIGalyser &figs,
               unsigned char* figdata,
               unsigned char figlen,
               unsigned short int figtype,
               unsigned short int indent);

int eti_analyse(eti_analyse_config_t& config);

std::string get_fig_0_13_userapp(int user_app_type)
{
    switch (user_app_type) {
        case 0x000: return "Reserved for future definition";
        case 0x001: return "Not used";
        case 0x002: return "MOT Slideshow";
        case 0x003: return "MOT Broadacst Web Site";
        case 0x004: return "TPEG";
        case 0x005: return "DGPS";
        case 0x006: return "TMC";
        case 0x007: return "EPG";
        case 0x008: return "DAB Java";
        case 0x44a: return "Journaline";
        default: return "Reserved for future applications";
    }
}

#define no_argument 0
#define required_argument 1
#define optional_argument 2
const struct option longopts[] = {
    {"help",               no_argument,        0, 'h'},
    {"verbose",            no_argument,        0, 'v'},
    {"ignore-error",       no_argument,        0, 'e'},
    {"decode-stream",      required_argument,  0, 'd'},
    {"input",              required_argument,  0, 'i'}
};

void usage(void)
{
    fprintf(stderr,
            "ETISnoop analyser\n\n"
            "The ETSnoop analyser decodes and prints out a RAW ETI file in a\n"
            "form that makes analysis easier.\n"
            "Usage: etisnoop [-v] [-f] [-i filename] [-d stream_index]\n"
            "\n"
            "   -v      increase verbosity (can be given more than once)\n"
            "   -d N    decode subchannel N into .dabp, .aac and .wav files\n"
            "   -f      analyse FIC carousel\n");
}

int main(int argc, char *argv[])
{
    int index;
    int ch = 0;
    string file_name("-");
    map<int, DabPlusSnoop> streams_to_decode;

    verbosity = 0;
    bool ignore_error = false;
    bool analyse_fic_carousel = false;

    while(ch != -1) {
        ch = getopt_long(argc, argv, "d:efhvi:", longopts, &index);
        switch (ch) {
            case 'd':
                {
                    int subchix = atoi(optarg);
                DabPlusSnoop dps;
                streams_to_decode[subchix] = dps;
                }
                break;
            case 'e':
                ignore_error = true;
                break;
            case 'i':
                file_name = optarg;
                break;
            case 'f':
                analyse_fic_carousel = true;
                break;
            case 'v':
                verbosity++;
                break;
            case 'h':
                usage();
                return 1;
                break;
        }
    }

    FILE* etifd;

    if (file_name == "-") {
        printf("Analysing stdin\n");
        etifd = stdin;
    }
    else {
        etifd = fopen(file_name.c_str(), "r");
        if (etifd == NULL) {
            perror("File open failed");
            return 1;
        }
    }

    eti_analyse_config_t config = {
        .etifd = etifd,
        .ignore_error = ignore_error,
        .streams_to_decode = streams_to_decode,
        .analyse_fic_carousel = analyse_fic_carousel
    };
    eti_analyse(config);
    fclose(etifd);
}

int eti_analyse(eti_analyse_config_t& config)
{
    unsigned char p[ETINIPACKETSIZE];
    string desc;
    char prevsync[3]={0x00,0x00,0x00};
    unsigned char ficf,nst,fp,mid,ficl;
    unsigned short int fl,crch;
    unsigned short int crc;
    unsigned char scid,tpl,l1;
    unsigned short int sad[64],stl[64];
    char sdesc[256];

    bool running = true;

    int stream_type = ETI_STREAM_TYPE_NONE;
    if (identify_eti_format(config.etifd, &stream_type) == -1) {
        printf("Could not identify stream type\n");

        running = false;
    }
    else {
        printf("Identified ETI type ");
        if (stream_type == ETI_STREAM_TYPE_RAW)
            printf("RAW\n");
        else if (stream_type == ETI_STREAM_TYPE_STREAMED)
            printf("STREAMED\n");
        else if (stream_type == ETI_STREAM_TYPE_FRAMED)
            printf("FRAMED\n");
        else
            printf("?\n");
    }

    while (running) {

        int ret = get_eti_frame(config.etifd, stream_type, p);
        if (ret == -1) {
            fprintf(stderr, "ETI file read error\n");
            break;
        }
        else if (ret == 0) {
            fprintf(stderr, "End of ETI\n");
            break;
        }

        // SYNC
        printbuf("SYNC", 0, p, 4);

        // SYNC - ERR
        if (p[0] == 0xFF) {
            desc = "No error";
            printbuf("ERR", 1, p, 1, desc);
        }
        else {
            desc = "Error";
            printbuf("ERR", 1, p, 1, desc);
            if (!config.ignore_error) {
                printf("Aborting because of SYNC error\n");
                break;
            }
        }

        // SYNC - FSYNC

        if (memcmp(prevsync, "\x00\x00\x00", 3) == 0) {
            if ( (memcmp(p + 1, "\x07\x3a\xb6", 3) == 0) ||
                 (memcmp(p + 1, "\xf8\xc5\x49", 3) == 0) ) {
                desc = "OK";
                memcpy(prevsync, p+1, 3);
            }
            else {
                desc ="Wrong FSYNC";
                memcpy(prevsync, "\x00\x00\x00", 3);
            }
        } else if (memcmp(prevsync, "\x07\x3a\xb6", 3) == 0) {
            if (memcmp(p + 1, "\xf8\xc5\x49", 3) != 0) {
                desc = "Wrong FSYNC";
                memcpy(prevsync, "\x00\x00\x00", 3);
            } else {
                desc = "OK";
                memcpy(prevsync, p + 1, 3);
            }
        } else if (memcmp(prevsync, "\xf8\xc5\x49", 3) == 0) {
            if (memcmp(p + 1, "\x07\x3a\xb6", 3) != 0) {
                desc = "Wrong FSYNC";
                memcpy(prevsync, "\x00\x00\x00", 3);
            } else {
                desc = "OK";
                memcpy(prevsync, p + 1, 3);
            }
        }
        printbuf("Sync FSYNC", 1, p + 1, 3, desc);

        // LIDATA
        printbuf("LDATA", 0, NULL, 0);
        // LIDATA - FC
        printbuf("FC - Frame Characterization field", 1, p+4, 4);
        // LIDATA - FC - FCT
        char fct[25];
        sprintf(fct, "%d", p[4]);
        printbuf("FCT  - Frame Count", 2, p+4, 1, fct);
        // LIDATA - FC - FICF
        ficf = (p[5] & 0x80) >> 7;

        {
            stringstream ss;
            ss << (int)ficf;
            if (ficf == 1) {
                ss << "- FIC Information are present";
            }
            else {
                ss << "- FIC Information are not present";
            }

            printbuf("FICF - Fast Information Channel Flag", 2, NULL, 0, ss.str());
        }

        // LIDATA - FC - NST
        nst = p[5] & 0x7F;
        {
            stringstream ss;
            ss << (int)nst;
            printbuf("NST  - Number of streams", 2, NULL, 0, ss.str());
        }

        // LIDATA - FC - FP
        fp = (p[6] & 0xE0) >> 5;
        {
            stringstream ss;
            ss << (int)fp;
            printbuf("FP   - Frame Phase", 2, &fp, 1, ss.str());
        }

        // LIDATA - FC - MID
        mid = (p[6] & 0x18) >> 3;
        {
            stringstream ss;
            ss << "Mode ";
            if (mid != 0) {
                ss << (int)mid;
            }
            else {
                ss << "4";
            }
            printbuf("MID  - Mode Identity", 2, &mid, 1, ss.str());
        }

        // LIDATA - FC - FL
        fl = (p[6] & 0x07) * 256 + p[7];
        {
            stringstream ss;
            ss << fl << " words";
            printbuf("FL   - Frame Length", 2, NULL, 0, ss.str());
        }

        if (ficf == 0) {
            ficl = 0;
        }
        else if (mid == 3) {
            ficl = 32;
        }
        else {
            ficl = 24;
        }

        // STC
        printbuf("STC - Stream Characterisation", 1, NULL, 0);

        for (int i=0; i < nst; i++) {
            sprintf(sdesc, "Stream number %d", i);
            printbuf("STC  - Stream Characterisation", 2, p + 8 + 4*i, 4, sdesc);
            scid = (p[8 + 4*i] & 0xFC) >> 2;
            sprintf(sdesc, "%d", scid);
            printbuf("SCID - Sub-channel Identifier", 3, NULL, 0, sdesc);
            sad[i] = (p[8+4*i] & 0x03) * 256 + p[9+4*i];
            sprintf(sdesc, "%d", sad[i]);
            printbuf("SAD  - Sub-channel Start Address", 3, NULL, 0, sdesc);
            tpl = (p[10+4*i] & 0xFC) >> 2;

            if ((tpl & 0x20) >> 5 == 1) {
                unsigned char opt, plevel;
                string plevelstr;
                opt = (tpl & 0x1c) >> 2;
                plevel = (tpl & 0x03);
                if (opt == 0x00) {
                    if (plevel == 0)
                        plevelstr = "1-A, 1/4, 16 CUs";
                    else if (plevel == 1)
                        plevelstr = "2-A, 3/8, 8 CUs";
                    else if (plevel == 2)
                        plevelstr = "3-A, 1/2, 6 CUs";
                    else if (plevel == 3)
                        plevelstr = "4-A, 3/4, 4 CUs";
                }
                else if (opt == 0x01) {
                    if (plevel == 0)
                        plevelstr = "1-B, 4/9, 27 CUs";
                    else if (plevel == 1)
                        plevelstr = "2-B, 4/7, 21 CUs";
                    else if (plevel == 2)
                        plevelstr = "3-B, 4/6, 18 CUs";
                    else if (plevel == 3)
                        plevelstr = "4-B, 4/5, 15 CUs";
                }
                else {
                    stringstream ss;
                    ss << "Unknown option " << opt;
                    plevelstr = ss.str();
                }
                sprintf(sdesc, "0x%02x - Equal Error Protection. %s", tpl, plevelstr.c_str());
            }
            else {
                unsigned char tsw, uepidx;
                tsw = (tpl & 0x08);
                uepidx = tpl & 0x07;
                sprintf(sdesc, "0x%02x - Unequal Error Protection. Table switch %d,  UEP index %d", tpl, tsw, uepidx);
            }
            printbuf("TPL  - Sub-channel Type and Protection Level", 3, NULL, 0, sdesc);
            stl[i] = (p[10+4*i] & 0x03) * 256 + \
                      p[11+4*i];
            sprintf(sdesc, "%d => %d kbit/s", stl[i], stl[i]*8/3);
            printbuf("STL  - Sub-channel Stream Length", 3, NULL, 0, sdesc);

            if (config.streams_to_decode.count(i) > 0) {
                config.streams_to_decode[i].set_subchannel_index(stl[i]/3);
                config.streams_to_decode[i].set_index(i);
            }
        }

        // EOH
        printbuf("EOH - End Of Header", 1, p + 8 + 4*nst, 4);
        unsigned short int mnsc = p[8 + 4*nst] * 256 + \
                                  p[8 + 4*nst + 1];
        {
            stringstream ss;
            ss << mnsc;
            printbuf("MNSC - Multiplex Network Signalling Channel", 2, p+8+4*nst, 2, ss.str());
        }

        crch = p[8 + 4*nst + 2]*256 + \
               p[8 + 4*nst + 3];
        crc  = 0xffff;

        for (int i=4; i < 8 + 4*nst + 2; i++)
            crc = update_crc_ccitt(crc, p[i]);
        crc =~ crc;

        if (crc == crch) {
            sprintf(sdesc,"CRC OK");
        }
        else {
            sprintf(sdesc,"CRC Mismatch: %02x",crc);
        }

        printbuf("Header CRC", 2, p + 8 + 4*nst + 2, 2, sdesc);

        // MST - FIC
        if (ficf == 1) {
            int endmarker = 0;
            int figcount = 0;
            unsigned char *fib, *fig;
            unsigned short int figcrc;

            FIGalyser figs;

            unsigned char ficdata[32*4];
            memcpy(ficdata, p + 12 + 4*nst, ficl*4);
            sprintf(sdesc, "FIC Data (%d bytes)", ficl*4);
            //printbuf(sdesc, 1, ficdata, ficl*4);
            printbuf(sdesc, 1, NULL, 0);
            fib = p + 12 + 4*nst;
            for(int i = 0; i < ficl*4/32; i++) {
                fig=fib;
                figs.set_fib(i);
                endmarker=0;
                figcount=0;
                while (!endmarker) {
                    unsigned char figtype, figlen;
                    figtype = (fig[0] & 0xE0) >> 5;
                    if (figtype != 7) {
                        figlen = fig[0] & 0x1F;
                        sprintf(sdesc, "FIG %d [%d bytes]", figtype, figlen);
                        printbuf(sdesc, 3, fig+1, figlen);
                        decodeFIG(figs, fig+1, figlen, figtype, 4);
                        fig += figlen + 1;
                        figcount += figlen + 1;
                        if (figcount >= 29)
                            endmarker = 1;
                    }
                    else {
                        endmarker = 1;
                    }
                }
                figcrc = fib[30]*256 + fib[31];
                crc = 0xffff;
                for (int j = 0; j < 30; j++) {
                    crc = update_crc_ccitt(crc, fib[j]);
                }
                crc =~ crc;
                if (crc == figcrc)
                    sprintf(sdesc,"FIB CRC OK");
                else
                    sprintf(sdesc,"FIB CRC Mismatch: %02x",crc);

                printbuf("FIB CRC",3,fib+30,2,sdesc);
                fib += 32;
            }

            if (config.analyse_fic_carousel) {
                figs.analyse();
            }
        }

        int offset = 0;
        for (int i=0; i < nst; i++) {
            unsigned char streamdata[684*8];
            memcpy(streamdata, p + 12 + 4*nst + ficf*ficl*4 + offset, stl[i]*8);
            offset += stl[i] * 8;
            if (config.streams_to_decode.count(i) > 0) {
                sprintf(sdesc, "id %d, len %d, selected for decoding", i, stl[i]*8);
            }
            else {
                sprintf(sdesc, "id %d, len %d, not selected for decoding", i, stl[i]*8);
            }
            if (verbosity > 1) {
                printbuf("Stream Data", 1, streamdata, stl[i]*8, sdesc);
            }
            else {
                printbuf("Stream Data", 1, streamdata, 0, sdesc);
            }

            if (config.streams_to_decode.count(i) > 0) {
                config.streams_to_decode[i].push(streamdata, stl[i]*8);
            }

        }

        // EOF
        crch = p[12 + 4*nst + ficf*ficl*4 + offset] * 256 + \
               p[12 + 4*nst + ficf*ficl*4 + offset + 1];

        crc = 0xffff;

        for (int i = 12 + 4*nst; i < 12 + 4*nst + ficf*ficl*4 + offset; i++)
            crc = update_crc_ccitt(crc, p[i]);
        crc =~ crc;
        if (crc == crch)
            sprintf(sdesc, "CRC OK");
        else
            sprintf(sdesc, "CRC Mismatch: %02x", crc);

        printbuf("EOF", 1, p + 12 + 4*nst + ficf*ficl*4 + offset, 4);
        printbuf("CRC", 2, p + 12 + 4*nst + ficf*ficl*4 + offset, 2, sdesc);

        //RFU
        printbuf("RFU", 2, p + 12 + 4*nst + ficf*ficl*4 + offset + 2, 2);

        //TIST
        l1 = (p[12 + 4*nst + ficf*ficl*4 + offset + 5] & 0xfe) >> 1;
        sprintf(sdesc, "%d ms", l1*8);
        printbuf("TIST - Time Stamp", 1, p+12+4*nst+ficf*ficl*4+offset+4, 4, sdesc);


        if (verbosity) {
            printf("-------------------------------------------------------------------------------------------------------------\n");
        }
    }


    std::map<int, DabPlusSnoop>::iterator it;
    for (it = config.streams_to_decode.begin();
            it != config.streams_to_decode.end();
            ++it) {
        it->second.close();
    }

    return 0;
}

void decodeFIG(FIGalyser &figs,
               unsigned char* f,
               unsigned char figlen,
               unsigned short int figtype,
               unsigned short int indent)
{
    char desc[256];

    switch (figtype) {
        case 0:
            {
                unsigned short int ext,cn,oe,pd;

                cn = (f[0] & 0x80) >> 7;
                oe = (f[0] & 0x40) >> 6;
                pd = (f[0] & 0x20) >> 5;
                ext = f[0] & 0x1F;
                sprintf(desc, "FIG %d/%d: C/N=%d OE=%d P/D=%d",
                        figtype, ext, cn, oe, pd);
                printbuf(desc, indent, f+1, figlen-1);

                figs.push_back(figtype, ext, figlen);

                switch (ext) {

                    case 0: // FIG 0/0
                        {
                            unsigned char cid, al, ch, hic, lowc, occ;
                            unsigned short int eid, eref;

                            eid  =  f[1]*256+f[2];
                            cid  = (f[1] & 0xF0) >> 4;
                            eref = (f[1] & 0x0F)*256 + \
                                    f[2];
                            ch   = (f[3] & 0xC0) >> 6;
                            al   = (f[3] & 0x20) >> 5;
                            hic  =  f[3] & 0x1F;
                            lowc =  f[4];
                            if (ch != 0) {
                                occ = f[5];
                                sprintf(desc,
                                        "Ensemble ID=0x%02x (Country id=%d, Ensemble reference=%d), Change flag=%d, Alarm flag=%d, CIF Count=%d/%d, Occurance change=%d",
                                        eid, cid, eref, ch, al, hic, lowc, occ);
                            }
                            else {
                                sprintf(desc,
                                        "Ensemble ID=0x%02x (Country id=%d, Ensemble reference=%d), Change flag=%d, Alarm flag=%d, CIF Count=%d/%d",
                                        eid, cid, eref, ch, al, hic, lowc);
                            }
                            printbuf(desc, indent+1, NULL, 0);

                        }
                        break;
                    case 1: // FIG 0/1 basic subchannel organisation
                        {
                            int i = 1;

                            while (i < figlen-3) {
                                // iterate over subchannels
                                int subch_id = f[i] >> 2;
                                int start_addr = ((f[i] & 0x03) << 8) |
                                                 (f[i+1]);
                                int long_flag  = (f[i+2] >> 7);

                                if (long_flag) {
                                    int option = (f[i+2] >> 4) & 0x07;
                                    int protection_level = (f[i+2] >> 2) & 0x03;
                                    int subchannel_size  = ((f[i+2] & 0x03) << 8 ) |
                                                           f[i+3];

                                    i += 4;

                                    if (option == 0x00) {
                                        sprintf(desc,
                                                "Subch 0x%x, start_addr %d, long, EEP %d-A, subch size %d",
                                                subch_id, start_addr, protection_level, subchannel_size);
                                    }
                                    else if (option == 0x01) {
                                        sprintf(desc,
                                                "Subch 0x%x, start_addr %d, long, EEP %d-B, subch size %d",
                                                subch_id, start_addr, protection_level, subchannel_size);
                                    }
                                    else {
                                        sprintf(desc,
                                                "Subch 0x%x, start_addr %d, long, invalid option %d, protection %d, subch size %d",
                                                subch_id, start_addr, option, protection_level, subchannel_size);
                                    }
                                }
                                else {
                                    int table_switch = (f[i+2] >> 6) & 0x01;
                                    unsigned int table_index  = (f[i+2] & 0x3F);


                                    if (table_switch == 0) {
                                        sprintf(desc,
                                                "Subch 0x%x, start_addr %d, short, table index %d",
                                                subch_id, start_addr, table_index);
                                    }
                                    else {
                                        sprintf(desc,
                                                "Subch 0x%x, start_addr %d, short, invalid table_switch(=1), table index %d",
                                                subch_id, start_addr, table_index);
                                    }

                                    i += 3;
                                }
                                printbuf(desc, indent+1, NULL, 0);
                            }

                        }
                        break;
                    case 2: // FIG 0/2
                        {
                            unsigned short int sref, sid;
                            unsigned char cid, ecc, local, caid, ncomp, timd, ps, ca, subchid, scty;
                            int k=1;
                            string psdesc;
                            char sctydesc[32];

                            while (k<figlen) {
                                if (pd == 0) {
                                    sid  =  f[k] * 256 + f[k+1];
                                    cid  = (f[k] & 0xF0) >> 4;
                                    sref = (f[k] & 0x0F) * 256 + f[k+1];
                                    k += 2;
                                }
                                else {
                                    sid  =  f[k] * 256 * 256 * 256 + \
                                            f[k+1] * 256 * 256 + \
                                            f[k+2] * 256 + \
                                            f[k+3];

                                    ecc  =  f[k];
                                    cid  = (f[k+1] & 0xF0) >> 4;
                                    sref = (f[k+1] & 0x0F) * 256 * 256 + \
                                           f[k+2] * 256 + \
                                           f[k+3];

                                    k += 4;
                                }

                                local = (f[k] & 0x80) >> 7;
                                caid  = (f[k] & 0x70) >> 4;
                                ncomp =  f[k] & 0x0F;

                                if (pd == 0)
                                    sprintf(desc,
                                            "Service ID=0x%02X (Country id=%d, Service reference=%d), Number of components=%d, Local flag=%d, CAID=%d",
                                            sid, cid, sref, ncomp, local, caid);
                                else
                                    sprintf(desc,
                                            "Service ID=0x%02X (ECC=%d, Country id=%d, Service reference=%d), Number of components=%d, Local flag=%d, CAID=%d",
                                            sid, ecc, cid, sref, ncomp, local, caid);
                                printbuf(desc, indent+1, NULL, 0);

                                k++;
                                for (int i=0; i<ncomp; i++) {
                                    unsigned char scomp[2];

                                    memcpy(scomp, f+k, 2);
                                    sprintf(desc, "Component[%d]", i);
                                    printbuf(desc, indent+2, scomp, 2, "");
                                    timd    = (scomp[0] & 0xC0) >> 6;
                                    ps      = (scomp[1] & 0x02) >> 1;
                                    ca      =  scomp[1] & 0x01;
                                    scty    =  scomp[0] & 0x3F;
                                    subchid = (scomp[1] & 0xFC) >> 2;

                                    /* useless, kept as reference
                                    if (timd == 3) {
                                        unsigned short int scid;
                                        scid = scty*64 + subchid;
                                    }
                                    */

                                    if (ps == 0) {
                                        psdesc = "Secondary service";
                                    }
                                    else {
                                        psdesc = "Primary service";
                                    }


                                    if (timd == 0) {
                                        //MSC stream audio
                                        if (scty == 0)
                                            sprintf(sctydesc, "MPEG Foreground sound (%d)", scty);
                                        else if (scty == 1)
                                            sprintf(sctydesc, "MPEG Background sound (%d)", scty);
                                        else if (scty == 2)
                                            sprintf(sctydesc, "Multi Channel sound (%d)", scty);
                                        else if (scty == 63)
                                            sprintf(sctydesc, "AAC sound (%d)", scty);
                                        else
                                            sprintf(sctydesc, "Unknown ASCTy (%d)", scty);

                                        sprintf(desc, "Stream audio mode, %s, %s, SubChannel ID=%02X, CA=%d", psdesc.c_str(), sctydesc, subchid, ca);
                                        printbuf(desc, indent+3, NULL, 0);
                                    }
                                    else if (timd == 1) {
                                        // MSC stream data
                                        sprintf(sctydesc, "DSCTy=%d", scty);
                                        sprintf(desc, "Stream data mode, %s, %s, SubChannel ID=%02X, CA=%d", psdesc.c_str(), sctydesc, subchid, ca);
                                        printbuf(desc, indent+3, NULL, 0);
                                    }
                                    else if (timd == 2) {
                                        // FIDC
                                        sprintf(sctydesc, "DSCTy=%d", scty);
                                        sprintf(desc, "FIDC mode, %s, %s, Fast Information Data Channel ID=%02X, CA=%d", psdesc.c_str(), sctydesc, subchid, ca);
                                        printbuf(desc, indent+3, NULL, 0);
                                    }
                                    else if (timd == 3) {
                                        // MSC Packet mode
                                        sprintf(desc, "MSC Packet Mode, %s, Service Component ID=%02X, CA=%d", psdesc.c_str(), subchid, ca);
                                        printbuf(desc, indent+3, NULL, 0);
                                    }
                                    k += 2;
                                }
                            }
                        }
                        break;
                    case 13: // FIG 0/13
                        {
                            uint32_t SId;
                            uint8_t  SCIdS;
                            uint8_t  No;

                            int k = 1;

                            if (pd == 0) { // Programme services, 16 bit SId
                                SId   = (f[k] << 8) |
                                         f[k+1];
                                k+=2;

                                SCIdS = f[k] >> 4;
                                No    = f[k] & 0x0F;
                                k++;
                            }
                            else { // Data services, 32 bit SId
                                SId   = (f[k]   << 24) |
                                        (f[k+1] << 16) |
                                        (f[k+2] << 8) |
                                         f[k+3];
                                k+=4;

                                SCIdS = f[k] >> 4;
                                No    = f[k] & 0x0F;
                                k++;

                            }

                            sprintf(desc, "FIG %d/%d: SId=%u SCIdS=%u No=%u",
                                    figtype, ext, SId, SCIdS, No);
                            printbuf(desc, indent+1, NULL, 0);

                            for (int numapp = 0; numapp < No; numapp++) {
                                uint16_t user_app_type = ((f[k] << 8) |
                                                         (f[k+1] & 0xE0)) >> 5;
                                uint8_t  user_app_len  = f[k+1] & 0x1F;
                                k+=2;

                                sprintf(desc, "User Application %d '%s'; length %u",
                                        user_app_type,
                                        get_fig_0_13_userapp(user_app_type).c_str(),
                                        user_app_len);
                                printbuf(desc, indent+2, NULL, 0);
                            }
                        }
                        break;
                }
            }
            break;

        case 1:
            {// SHORT LABELS
                unsigned short int ext,oe,charset;
                unsigned short int flag;
                char label[17];

                charset = (f[0] & 0xF0) >> 4;
                oe = (f[0] & 0x08) >> 3;
                ext = f[0] & 0x07;
                sprintf(desc,
                        "FIG %d/%d: OE=%d, Charset=%d",
                        figtype, ext, oe, charset);

                printbuf(desc, indent, f+1, figlen-1);
                memcpy(label, f+figlen-18, 16);
                label[16] = 0x00;
                flag = f[figlen-2] * 256 + \
                       f[figlen-1];

                figs.push_back(figtype, ext, figlen);

                switch (ext) {
                    case 0:
                        { // ENSEMBLE LABEL
                            unsigned short int eid;
                            eid = f[1] * 256 + f[2];
                            sprintf(desc, "Ensemble ID 0x%04X label: \"%s\", Short label mask: 0x%04X", eid, label, flag);
                            printinfo(desc, indent+1);
                        }
                        break;

                    case 1:
                        { // Programme LABEL
                            unsigned short int sid;
                            sid = f[1] * 256 + f[2];
                            sprintf(desc, "Service ID 0x%04X label: \"%s\", Short label mask: 0x%04X", sid, label, flag);
                            printinfo(desc, indent+1);
                        }
                        break;

                    case 4:
                        { // Service Component LABEL
                            unsigned int sid;
                            unsigned char pd, SCIdS;
                            pd    = (f[1] & 0x80) >> 7;
                            SCIdS =  f[1] & 0x0F;
                            if (pd == 0) {
                                sid = f[2] * 256 + \
                                      f[3];
                            }
                            else {
                                sid = f[2] * 256 * 256 * 256 + \
                                      f[3] * 256 * 256 + \
                                      f[4] * 256 + \
                                      f[5];
                            }
                            sprintf(desc,
                                    "Service ID  0x%08X , Service Component ID 0x%04X Short, label: \"%s\", label mask: 0x%04X",
                                    sid, SCIdS, label, flag);
                            printinfo(desc, indent+1);
                        }
                        break;

                    case 5:
                        { // Data Service LABEL
                            unsigned int sid;
                            sid = f[1] * 256 * 256 * 256 + \
                                  f[2] * 256 * 256 + \
                                  f[3] * 256 + \
                                  f[4];

                            sprintf(desc,
                                    "Service ID 0x%08X label: \"%s\", Short label mask: 0x%04X",
                                    sid, label, flag);
                            printinfo(desc, indent+1);
                        }
                        break;


                    case 6:
                        { // X-PAD User Application label
                            unsigned int sid;
                            unsigned char pd, SCIdS, xpadapp;
                            string xpadappdesc;

                            pd    = (f[1] & 0x80) >> 7;
                            SCIdS =  f[1] & 0x0F;
                            if (pd == 0) {
                                sid = f[2] * 256 + \
                                      f[3];
                                xpadapp = f[4] & 0x1F;
                            }
                            else {
                                sid = f[2] * 256 * 256 * 256 + \
                                      f[3] * 256 * 256 + \
                                      f[4] * 256 + \
                                      f[5];
                                xpadapp = f[6] & 0x1F;
                            }

                            if (xpadapp == 2) {
                                xpadappdesc = "DLS";
                            }
                            else if (xpadapp == 12) {
                                xpadappdesc = "MOT";
                            }
                            else {
                                xpadappdesc = "?";
                            }


                            sprintf(desc,"Service ID  0x%08X , Service Component ID 0x%04X Short, X-PAD App %02X (%s), label: \"%s\", label mask: 0x%04X",
                                    sid, SCIdS, xpadapp, xpadappdesc.c_str(), label, flag);
                            printbuf(desc,indent+1,NULL,0,"");
                        }
                        break;
                }
            }
            break;
        case 2:
            {// LONG LABELS
                unsigned short int ext,oe;

                uint8_t toggle_flag = (f[0] & 0x80) >> 7;
                uint8_t segment_index = (f[0] & 0x70) >> 4;
                oe = (f[0] & 0x08) >> 3;
                ext = f[0] & 0x07;
                sprintf(desc,
                        "FIG %d/%d: OE=%d, Segment_index=%d",
                        figtype, ext, oe, segment_index);

                printbuf(desc, indent, f+1, figlen-1);

                figs.push_back(figtype, ext, figlen);
            }
            break;
        case 5:
            {// FIDC
                unsigned short int ext;

                uint8_t d1 = (f[0] & 0x80) >> 7;
                uint8_t d2 = (f[0] & 0x40) >> 6;
                uint8_t tcid = (f[0] & 0x38) >> 5;
                ext = f[0] & 0x07;
                sprintf(desc,
                        "FIG %d/%d: D1=%d, D2=%d, TCId=%d",
                        figtype, ext, d1, d2, tcid);

                printbuf(desc, indent, f+1, figlen-1);

                figs.push_back(figtype, ext, figlen);
            }
            break;
        case 6:
            {// Conditional access
                fprintf(stderr, "ERROR: ETI contains unsupported FIG 6");
            }
            break;
    }
}


void printinfo(string header,
        int indent_level,
        int min_verb)
{
    if (verbosity >= min_verb) {
        for (int i = 0; i < indent_level; i++) {
            printf("\t");
        }
        printf("%s\n", header.c_str());
    }
}

void printbuf(string header,
        int indent_level,
        unsigned char* buffer,
        size_t size,
        string desc)
{
    if (verbosity > 0) {
        for (int i = 0; i < indent_level; i++) {
            printf("\t");
        }

        printf("%s", header.c_str());

        if (verbosity > 1) {
            if (size != 0) {
                printf(": ");
            }

            for (size_t i = 0; i < size; i++) {
                printf("%02x ", buffer[i]);
            }
        }

        if (desc != "") {
            printf(" [%s] ", desc.c_str());
        }

        printf("\n");
    }
}



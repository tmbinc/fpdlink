from migen import *
from migen.genlib.fsm import FSM, NextState
import random
import math
from misoc.interconnect.stream import Endpoint
from misoc.interconnect.csr import AutoCSR, CSRStorage, CSRStatus
from migen.genlib.fifo import *
from migen.genlib.cdc import BusSynchronizer, PulseSynchronizer

class Sampler(Module):
    def __init__(self):
        self.i = Signal(8)
        self.q = Signal(28)
        self.s = Signal(1)
        self.slip = Signal(1)
        
        self.adjust = Signal(16)
        self.adjust_inc = Signal(16)
        self.adjust_direction = Signal()
        
        self.adjptotal = Signal(32)
        self.adjmtotal = Signal(32)
        
        data_last = Signal(8)
        self.sync += data_last.eq(self.i)
        
        phase = Signal(3)
        self.phase = phase
        
        data = Cat(self.i, data_last)
        d = Array(data[i] for i in range(16))
        
        self.d = d
        
        bit = Signal()
        self.comb += bit.eq(d[phase])
        bit2 = Signal(1)
        self.comb += bit2.eq(d[phase+8])
        self.bit = bit
        self.bit2 = bit2
        
        edge_pre = Signal(16)
        self.comb += edge_pre.eq(Cat(0, data[:-1] ^ data[1:]))
        self.edge_pre = edge_pre
        
        edge_array = Array(edge_pre)
        edge = Signal(8)
        for i in range(8):
            self.comb += [
                edge[i].eq(edge_array[i + phase])
            ]
        
        pd = Signal(min=-3, max=3)
        self.pd = pd
        
        self.edge = Signal(8)
        self.sync += self.edge.eq(edge)
        
        self.edge1 = edge
        
        self.comb += [
            If(edge[7], pd.eq(3)).
            Elif(edge[6], pd.eq(2)).
            Elif(edge[5], pd.eq(1)).
            Elif(edge[2], pd.eq(-1)).
            Elif(edge[1], pd.eq(-2)).
            Elif(edge[0], pd.eq(-3)).
            Else(pd.eq(0))
        ]
        
        self.sync += [
            If(pd > 0, self.adjptotal.eq(self.adjptotal + 1)).
            Elif(pd < 0, self.adjmtotal.eq(self.adjmtotal + 1))
        ]
        
        
        adjust_next = Signal(len(self.adjust) + 1)
        adjust_overflow = Signal()
        self.comb += [
            adjust_next.eq(self.adjust + self.adjust_inc),
            adjust_overflow.eq(adjust_next[-1])
        ]
        self.sync += [
            self.adjust.eq(adjust_next),
        ]
        
        pd2p = Signal()
        self.comb += pd2p.eq(adjust_overflow & ~self.adjust_direction)

        pd2m = Signal()
        self.comb += pd2m.eq(adjust_overflow & self.adjust_direction)
        
        self.pd2p = pd2p
        self.pd2m = pd2m
        
        phase_ov = Signal(2)
        
        self.sync += [
            phase.eq(phase + pd + pd2p - pd2m),
            phase_ov.eq((phase + pd + pd2p - pd2m)[3:])
        ]
        
        numbits = Signal(max=31, reset=31)
        self.numbits =numbits
        sr = Signal(29)
        
        inc1 = Signal()
        inc2 = Signal()
        
        self.comb += [
            If(phase_ov == 0, inc1.eq(~self.slip), inc2.eq(0)).
            Elif(phase_ov == 1, inc1.eq(self.slip), inc2.eq(~self.slip)).
            Else(inc1.eq(0), inc2.eq(0)) # slip not supported in underflow case
        ]
        
        # shifter
        self.sync += [
            If(inc1, sr.eq(Cat(bit, sr[:-1]))).
            Elif(inc2, sr.eq(Cat(bit, bit2, sr[:-2])))
        ]
        
        s1 = Signal()
        s2 = Signal()
        s1d = Signal()
        s2d = Signal()
        
        self.sync += [
            s1.eq(0),
            s2.eq(0),
            s1d.eq(s1),
            s2d.eq(s2),
            If((numbits == 27) & inc1, s1.eq(1), numbits.eq(0)).
            Elif((numbits == 26) & inc2, s1.eq(1), numbits.eq(0)).
            Elif((numbits == 27) & inc2, s2.eq(1), numbits.eq(1)).
            Elif(inc1, numbits.eq(numbits + 1)).
            Elif(inc2, numbits.eq(numbits + 2))
        ]
        
        self.sr = Signal(8)
        self.comb += self.sr.eq(sr[:8])
        
        self.comb += [
            self.s.eq(s1 | s2),
            If(s1, self.q.eq(sr[:28])).
            Elif(s2, self.q.eq(sr[1:29]))
        ]

class Decoder(Module):
    def __init__(self):
        
        self.i = Signal(28)
        self.s = Signal()
        self.slip = Signal(1)
        self.q = Signal(32)
        self.qs = Signal()
       
        self.lock = Signal()
        
        c1 = self.i[0]
        dca = self.i[13]
        dcb = self.i[14]
        c0 = self.i[27]
        
        data = Cat(self.i[1:13], self.i[15:27])
        
        # sync using C1/C2.
        syncok = c1 &~ c0
        
        syncfail = Signal(max=3)
        
        self.sync += [
            self.slip.eq(0),
            If(self.s &~ syncok, 
                If(syncfail == 3, syncfail.eq(0), self.slip.eq(1)).
                Else(syncfail.eq(syncfail + 1))
            )
            ]
        
        # 
        
        dca_state = Signal(32)
        
        self.sync += [
            If(self.s, dca_state.eq(Cat(dca, dca_state[:-1])))
        ]
        
        dca_state_decoded = Signal(5)
        dcas = [
                0x1999999e, 0x3333333c, 0x66666678, 0xccccccf0, 0x999999e1, 0x333333c3, 0x66666786, 0xcccccf0c, 
                0x99999e19, 0x33333c33, 0x66667866, 0xccccf0cc, 0x9999e199, 0x3333c333, 0x66678666, 0xcccf0ccc, 
                0x999e1999, 0x333c3333, 0x66786666, 0xccf0cccc, 0x99e19999, 0x33c33333, 0x67866666, 0xcf0ccccc, 
                0x9e199999, 0x3c333333, 0x78666666, 0xf0cccccc, 0xe1999999, 0xc3333333, 0x86666667, 0x0ccccccf
        ]
        
        dca_state_decoded_id = Signal(32)
        
        self.sync += [
                If(self.s, self.lock.eq(dca_state_decoded_id != 0))
            ]
        
        for i, j in enumerate(dcas):
            self.comb += [
                dca_state_decoded_id[i].eq(dca_state == j)
            ]

            self.sync += [
                # 2 cycles behind (dca_state shift register, dca_state_decoded)
                If(self.s & (dca_state == j), dca_state_decoded.eq((i+2)%32))
            ]

        order = [[int(x, 10) for x in l.strip().replace("??", "-1").replace(" ", "")[:-1].split(",")] for l in open("allbits").readlines()]
        
        invert_indicator = [-1 for i in range(32)]
        
        for i in range(32):
                u = None
                used = 0
                for j in range(24):
                        if order[j][i] != -1:
                                used |= 1<<order[j][i]
                        else:
                                u = j
                used ^= 0xFFFFFF
                if used:
                        n = int(math.log2(used))
                        assert 1<<n == (used), "more than 1 unused bit"
                        invert_indicator[i] = n
                        order[u][i] = n
        
        self.data_before_reorder = Signal(24)
        do_invert = Signal(24)
        self.comb += self.data_before_reorder.eq(data ^ Replicate(dcb, 24) ^ do_invert)
        
        self.reassembled = Array([Signal(24) for i in range(32)])
        self.invert = Array(Signal(24) for i in range(32))
        for i in range(32):
            self.comb += [
                self.reassembled[i][j].eq(self.data_before_reorder[order[j][i]]) for j in range(24)] + [
                self.invert[i][j].eq((dcb ^ data[invert_indicator[i]]) if invert_indicator[i] not in [-1, j]else 0)
                for j in range(24)
            ]
        
        
        self.do_invert = do_invert
        self.comb += do_invert.eq(self.invert[dca_state_decoded])
        
        data_reassembled = self.reassembled[dca_state_decoded]

        self.sync += self.q.eq(data_reassembled)
        self.sync += self.qs.eq(self.s)
        
        # sequence counter...
        self.scnt = Signal(7)
        self.sync += [ If (self.s, self.scnt.eq(self.scnt + 1)) ]
        self.sync += self.q[24:31].eq(self.scnt)
        self.sync += self.q[31].eq(self.lock)


class FpdTop(Module, AutoCSR):
    def __init__(self, lvds_in, _debug, sim = False):
        self._adjust = CSRStorage(16, atomic_write=True)
        self._adjust_direction = CSRStorage(1)
        
        self._adjptotal = CSRStatus(32)
        self._adjmtotal = CSRStatus(32)
        
        self._lock = CSRStatus(1)
        
        self._invert = CSRStorage(1)
        self._update_counter = CSRStorage(1)
        
        self._debug_mux = CSRStorage(8)
        
        self.submodules.adjptotal_sync = BusSynchronizer(32, "pix1x", "sys")
        self.submodules.adjmtotal_sync = BusSynchronizer(32, "pix1x", "sys")
    
        # IBUFDS        	
        lvds_se = Signal()
        if not sim:
            self.specials += Instance("IBUFDS", i_I=lvds_in[0], i_IB=lvds_in[1], o_O=lvds_se)

        debug = Signal(len(_debug))
        self.comb += _debug.eq(debug)
        
        d0 = Signal()
        self.sync.pix1x += [
            d0.eq(~d0)
        ]
        
        # SERDES
        self.i = Signal(8)
        
        self.comb += debug[0].eq(lvds_se)

        pd_valid = Signal()
        pd_incdec = Signal()
        pd_edge = Signal()
        pd_cascade = Signal()
        self.serdesstrobe = Signal()
        if not sim:
            self.specials += Instance("ISERDES2",
                                      p_SERDES_MODE="MASTER",
                                      p_BITSLIP_ENABLE="FALSE", p_DATA_RATE="SDR", p_DATA_WIDTH=8,
                                      p_INTERFACE_TYPE="RETIMED",

                                      i_D=lvds_se,
                                      o_Q4=self.i[7], o_Q3=self.i[6], o_Q2=self.i[5], o_Q1=self.i[4],

                                      i_BITSLIP=0, i_CE0=1, i_RST=0,
                                      i_CLK0=ClockSignal("pix8x"), i_CLKDIV=ClockSignal("pix1x"),
                                      i_IOCE=self.serdesstrobe,

                                      o_VALID=pd_valid, o_INCDEC=pd_incdec,
                                      i_SHIFTIN=pd_edge, o_SHIFTOUT=pd_cascade)
            self.specials += Instance("ISERDES2",
                                      p_SERDES_MODE="SLAVE",
                                      p_BITSLIP_ENABLE="FALSE", p_DATA_RATE="SDR", p_DATA_WIDTH=8,
                                      p_INTERFACE_TYPE="RETIMED",

                                      i_D=lvds_se,
                                      o_Q4=self.i[3], o_Q3=self.i[2], o_Q2=self.i[1], o_Q1=self.i[0],

                                      i_BITSLIP=0, i_CE0=1, i_RST=0,
                                      i_CLK0=ClockSignal("pix8x"), i_CLKDIV=ClockSignal("pix1x"),
                                      i_IOCE=self.serdesstrobe,
                                      i_SHIFTIN=pd_cascade, o_SHIFTOUT=pd_edge)

        self.lock = Signal()        
        self.submodules.sampler = ClockDomainsRenamer({"sys": "pix1x"})(Sampler())

        fifo = AsyncFIFO(28, 64)
        self.submodules.fifo = ClockDomainsRenamer(
            {"write":"pix1x", "read":"sys"})(fifo)

        self.submodules.slip = PulseSynchronizer(idomain = "sys", odomain = "pix1x")

        self.submodules.decoder = Decoder()
        
        # CDC for adjust set
        self.submodules.adjust_sync = BusSynchronizer(1 + 16, "sys", "pix1x")
        self.comb += [
            self.adjust_sync.i[-1].eq(self._adjust_direction.storage),
            self.adjust_sync.i[:16].eq(self._adjust.storage),
            self.sampler.adjust_direction.eq(self.adjust_sync.o[-1]),
            self.sampler.adjust_inc.eq(self.adjust_sync.o[:16]),
        ]
        
        self.comb += [
            # SERDES -> sampler
#            If(self._invert.storage,
#              self.sampler.i.eq(self.i[::-1]))
#            .Else(
#              self.sampler.i.eq(~self.i[::-1])),
            self.sampler.i.eq(self.i[::-1]),
            
            # sampler -> fifo
            self.fifo.din.eq(self.sampler.q),
            self.fifo.we.eq(self.sampler.s),
            self.fifo.re.eq(1),
            
            # fifo -> decoder
            self.decoder.i.eq(self.fifo.dout[::-1]),
            self.decoder.s.eq(self.fifo.readable),
            
            # decoder slip request -> pulse sync -> sampler slip
            self.slip.i.eq(self.decoder.slip),
            self.sampler.slip.eq(self.slip.o),

#            self.source.stb.eq(self.decoder.qs),
#            self.source.d.eq(self.decoder.q[24:32]),
            self.lock.eq(self.decoder.lock),
        ]
        
        # capture decoder output on decoder.qs
        self.decoder_q = Signal(32)
        self.sync += [
            If(self.decoder.qs,
              self.decoder_q.eq(self.decoder.q)
            )
        ]

#        self.comb += debug[11].eq(self.source.stb)
#        self.comb += debug[9].eq(self.lock)
        self.samplerq = Signal(28)
        self.submodules.samplerq_sync = BusSynchronizer(idomain = "pix1x", odomain = "sys", width=len(self.samplerq))
        self.comb += [
          self.samplerq_sync.i.eq(self.sampler.q),
          self.samplerq.eq(self.samplerq_sync.o)
        ]
        
        self.comb += [ 
          If(self._debug_mux.storage == 0,
              debug[1:9].eq(self.sampler.edge)).
          Elif(self._debug_mux.storage == 1,
            debug[1:9].eq(self.decoder.i[0:8])).
          Elif(self._debug_mux.storage == 2,
            debug[1:9].eq(self.decoder.i[8:16])).
          Elif(self._debug_mux.storage == 3,
            debug[1:9].eq(self.decoder.i[16:24])).
          Elif(self._debug_mux.storage == 4,
            debug[1:9].eq(self.decoder.i[24:28])).
          Elif(self._debug_mux.storage == 5,
            debug[1:9].eq(self.samplerq[0:8])).
          Elif(self._debug_mux.storage == 6,
            debug[1:9].eq(self.samplerq[8:16])).
          Elif(self._debug_mux.storage == 7,
            debug[1:9].eq(self.samplerq[16:24])).
          Elif(self._debug_mux.storage == 8,
            debug[1:9].eq(self.samplerq[24:28])).
          Elif(self._debug_mux.storage == 9,
            debug[1:9].eq(self.sampler.sr))
          ]
#        self.comb += debug[2].eq(self.sampler.numbits[0])
        self.comb += debug[0].eq(d0)

#        self.comb += debug[6].eq(self.i[0])
#        self.comb += debug[8].eq(self.sampler.s)
        self.comb += debug[10].eq(self.fifo.readable)
        self.comb += debug[11].eq(self.decoder.lock)
        self.comb += debug[12].eq(self.decoder.qs)

#        self.comb += debug[10].eq(self.sampler.phase[0])
#        self.comb += debug[11].eq(self.sampler.phase[1])
#        self.comb += debug[12].eq(self.sampler.phase[2])
        
        self.comb += self._lock.status.eq(self.decoder.lock)

        # status (via synchronizer)
        self.comb += [
            self.adjptotal_sync.i.eq(self.sampler.adjptotal),
            self.adjmtotal_sync.i.eq(self.sampler.adjmtotal),
        ]
        
        self.sync += [
            If(self._update_counter.storage, 
                self._adjptotal.status.eq(self.adjptotal_sync.o),
                self._adjmtotal.status.eq(self.adjmtotal_sync.o),
            )
        ]

        # output
        self.source = Endpoint([('d', 8)])
        
        self.submodules.fifo_read_fsm = FSM()
        self.fifo_read_fsm.act("B0",
            If (self.source.ack & self.decoder.qs & self.decoder.lock,
                NextState("B1")
            ),
            self.source.stb.eq(self.decoder.qs & self.decoder.lock,),
            self.source.payload.d.eq(Cat(self.decoder.q[8:14], self.decoder.q[15:17]))
        )

        self.fifo_read_fsm.act("B1",
            If (self.source.ack,
                NextState("B0")
            ),
            self.source.stb.eq(1),
            self.source.payload.d.eq(self.decoder_q[0:8])
        )

class FpdTopTest(Module):
    def __init__(self):
        debug = Signal(14)
        lvds_in = Signal(2)
        self.clock_domains.cd_pix1x = ClockDomain()
        self.submodules.fpdtop = FpdTop(lvds_in, debug, sim = True)

if __name__ == '__main__':
    def testbench_csv(dut):
        phase_out = open("phase_out_hw", "w")
        for i in open("bits.txt"):
            data = int(i.strip(), 0x10)
            d = 0
            for i in range(8):
                d |= ((data >> i) & 1) << (7-i)
#            print("%02x" % data, end=' ')
            phase_out.write("%d %d\n" % ((yield dut.sampler.phase), (yield dut.sampler.numbits)))
            yield dut.i.eq(data)

            if (yield dut.sampler.s):
#                print("%07x" % (yield dut.q))
                v = yield dut.sampler.q
                print(''.join("%d" % ((v >> i)&1) for i in range(28)), end=' ')
                print("%07x" % v)
            yield

    def testbench_sampler(dut):
        yield dut.adjust_inc.eq(0x5000)
        phase_out = open("phase_out_hw", "w")
        for i in open("bits.txt"):
            data = int(i.strip(), 0x10)
            d = 0
            for i in range(8):
                d |= ((data >> i) & 1) << (7-i)
            
            yield dut.i.eq(d)
            
            d = (yield dut.d)
            edge = (yield dut.edge1)

            phase = (yield dut.phase)
            print(''.join("%d"% d[(15-i)] for i in range(16) ), end=' ')
            print((yield dut.bit), end=' ')
            print(''.join("%d"% d[(15-i)] if ((15-i) != phase) else "x" for i in range(16) ), end=' ')
            print(''.join("%d"% ((edge>>(15-i))&1) for i in range(0, 16)), end=' ')
            print("%02x" % (edge & 0xFF), end=' ')
            print(''.join(" x"[((edge >> (7-i))&1)] for i in range(8)), end=' ')
            print(["<<<", "<< ", "<  ", "   ", "  >", " >>", ">>>"][(yield dut.pd) + 3], end=' ')
            print("%04x" % ((yield dut.adjust)), "adj", "%-3d %-3d"%  ((yield dut.pd2p), (yield dut.pd2m)), end=' ')
            print()
            phase_out.write("%d %d\n" % ((yield dut.phase), (yield dut.numbits)))


            if (yield dut.s):
#                print("%07x" % (yield dut.q))
                v = yield dut.q
                print('WORD', end=' ')
                print(''.join("%d" % ((v >> i)&1) for i in range(28)), end=' ')
                print("%07x" % v)
            yield

    def testbench_bits(dut):
        for i in open("test3.bit").read():
            yield dut.i.eq(i == "0" and 0xFF or 0x00)
            
            if (yield dut.sampler.s):
#                print("%07x" % (yield dut.q))
                v = yield dut.sampler.q
                print(''.join("%d" % ((v >> i)&1) for i in range(28)), end=' ')
                print("%07x" % v)
            yield

    def testbench_words(dut):
        for i in open("words"):
            word, data = i.strip().split()
            word = int(word, 0x10)
            data = int(data, 0x10)
            yield dut.i.eq(word)
            yield dut.s.eq(1)
            yield
            yield dut.s.eq(0)
            yield
            v = (yield dut.q)
            do_invert = (yield dut.do_invert)
            lock = (yield dut.lock)
            for i in range(random.randint(0, 10)):
                yield
            print("%07x %06x <> %06x (%06x) (do_invert=%d, lock=%d)" % (word, data, v, v ^ data, do_invert, lock))

    def testbench_top(dut):
        lim = 0
        for i in open("bits.txt"):
            data = int(i.strip(), 0x10)
            d = 0
            for i in range(8):
                d |= ((data >> i) & 1) << (7-i)   
            yield dut.fpdtop.i.eq(d)
            print("%02x" % d)
            yield

    print("Running simulation...")
    #t = Sampler()
    #run_simulation(t, testbench_sampler(t), vcd_name='sampler.vcd')
    #print("Running simulation...")
    #t = Decoder()
    #run_simulation(t, testbench_words(t), vcd_name='sampler.vcd')
    
    t = FpdTopTest()
    run_simulation(t, testbench_top(t), vcd_name='sampler.vcd')


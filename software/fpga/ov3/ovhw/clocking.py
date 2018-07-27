from migen import *

class ClockGen(Module):
    def __init__(self, clkin):
        self.clk_sys = Signal()
        self.clk_sdram = Signal()
        self.clk_sdram_sample = Signal()
        self.clk_pix1x = Signal()
        self.clk_pix8x = Signal()
        self.cd_sys = ClockDomain()
        self.cd_pix1x = ClockDomain()
        self.cd_pix8x = ClockDomain()
        self.pll_locked = Signal()

        clkout0, clkout2, clkout3, clkout4, clkout5 = Signal(), Signal(), Signal(), Signal(), Signal()
        dcm_locked = Signal()
        clkdcm = Signal()

        feedback = Signal()

        # Simple 2X: 12MHz -> 24MHz
        self.specials.dcm = Instance("DCM_SP",
            Instance.Input("CLKIN", clkin),
            Instance.Input("CLKFB", clkdcm),
            Instance.Input("RST", 0),
            Instance.Input("PSEN", 0),
            Instance.Output("CLK2X", clkdcm),
            Instance.Output("LOCKED", dcm_locked),
            Instance.Parameter("CLK_FEEDBACK", "2X"),
        )

        # VCO 400.1000MHz
        # PFD 19..400MHz
        # 24MHz in, /1 24MHz PFD, x25 600MHz VCO, /6 100MHz CLKOUT
        self.specials.pll = Instance("PLL_BASE",
            Instance.Input("CLKIN", clkdcm),
            Instance.Input("CLKFBIN", feedback),
            Instance.Input("RST", ~dcm_locked),
            Instance.Output("CLKFBOUT", feedback),
            Instance.Output("CLKOUT0", clkout0),
            Instance.Output("CLKOUT1", self.clk_sdram),
            Instance.Output("CLKOUT2", clkout2),
            Instance.Output("CLKOUT3", clkout3),
            Instance.Output("CLKOUT4", clkout4),
            Instance.Output("CLKOUT5", clkout5),
            Instance.Output("LOCKED", self.pll_locked),
            Instance.Parameter("BANDWIDTH", "LOW"),
            Instance.Parameter("COMPENSATION", "DCM2PLL"),
            Instance.Parameter("CLK_FEEDBACK", "CLKFBOUT"),
            Instance.Parameter("DIVCLK_DIVIDE", 1),
            Instance.Parameter("CLKFBOUT_MULT", 45),
            Instance.Parameter("CLKOUT0_DIVIDE", 1) ,    # pix8x
            Instance.Parameter("CLKOUT0_PHASE", 0.0),
            Instance.Parameter("CLKOUT1_DIVIDE", 12),    # SDRAM
            Instance.Parameter("CLKOUT1_PHASE", 180.0),
            Instance.Parameter("CLKOUT2_DIVIDE", 12),    # SRAM sample
            Instance.Parameter("CLKOUT2_PHASE", 180.0),
            Instance.Parameter("CLKOUT3_DIVIDE", 8),     # pix1x
            Instance.Parameter("CLKOUT3_PHASE", 0.0),
            Instance.Parameter("CLKOUT4_DIVIDE", 12),    # sysclk
            Instance.Parameter("CLKOUT4_PHASE", 0.0),
        )
        locked_async = Signal()
        self.serdesstrobe = Signal()

        self.specials += [
            Instance("BUFG", Instance.Input("I", clkout4),
                             Instance.Output("O", self.clk_sys)),
            Instance("BUFG", Instance.Input("I", clkout2),
                             Instance.Output("O", self.clk_sdram_sample)),
            Instance("BUFG", Instance.Input("I", clkout3),
                             Instance.Output("O", self.clk_pix1x)),
#            Instance("BUFG", Instance.Input("I", clkout5),
#                             Instance.Output("O", self.clk_pix4xn)),
#            Instance("BUFG", Instance.Input("I", clkout3),
#                             Instance.Output("O", self.clk_pix1x)),

            Instance("BUFPLL", p_DIVIDE=8,
                     i_PLLIN=clkout0, i_GCLK=ClockSignal("pix1x"), i_LOCKED=self.pll_locked,
                     o_IOCLK=self.clk_pix8x, o_LOCK=locked_async, o_SERDESSTROBE=self.serdesstrobe),
        ]

        # Reset generator: 4 cycles in reset after PLL is locked
        rst_ctr = Signal(max=4)
        self.clock_domains.cd_rst = ClockDomain()
        self.cd_sys.rst.reset = 1
        self.sync.rst += If(rst_ctr == 3,
                            self.cd_sys.rst.eq(0)
                         ).Else(
                            rst_ctr.eq(rst_ctr+1)
                         )
        self.comb += [
            self.cd_rst.clk.eq(self.clk_sys),
            self.cd_rst.rst.eq(~self.pll_locked),
            self.cd_sys.clk.eq(self.clk_sys),
            self.cd_pix1x.clk.eq(self.clk_pix1x),
            self.cd_pix1x.rst.eq(self.cd_sys.rst),
            self.cd_pix8x.clk.eq(self.clk_pix8x),
            self.cd_pix8x.rst.eq(self.cd_sys.rst),
        ]

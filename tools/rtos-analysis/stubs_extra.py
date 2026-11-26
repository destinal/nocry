# functions the emulated CPU cannot run or that wait on the scheduler
def note(name, v=0):
    def f(uc):
        out('# stub %s(%#x, %#x)' % (name, uc.reg_read(UC_MIPS_REG_A0), uc.reg_read(UC_MIPS_REG_A1))); ret(uc, v)
    return f
def quiet(v=0):
    def f(uc): ret(uc, v)
    return f
EXTRA_RET = {0x86001968: 32, 0x8600e908: 1}
EXTRA = {

    0x86001984: note('cache_op_range_a'), 0x86001a3c: note('cache_op_range_b'), 0x86001af4: note('icache_flush_all'),
    0x86001b3c: note('dcache_flush_all'), 0x86001b84: note('dcache_scache_flush_all'), 0x86001bfc: note('cache_fn_1bfc'),

    0x86001968: quiet(),   # cache line size

    0x86001c18: note("cache_size_init"),
    0x8600e2a0: quiet(),   # os lock (scheduler suspend / critical section)
    0x8600e304: quiet(),   # os unlock

}

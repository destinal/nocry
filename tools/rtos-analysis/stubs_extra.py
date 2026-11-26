# functions the emulated CPU cannot run or that wait on the scheduler
def note(name, v=0):
    def f(uc):
        out('# stub %s(%#x, %#x)' % (name, uc.reg_read(UC_MIPS_REG_A0), uc.reg_read(UC_MIPS_REG_A1))); ret(uc, v)
    return f
def quiet(v=0):
    def f(uc): ret(uc, v)
    return f
EXTRA_RET = {A['cache_linesz']: 32, A['wait_event']: 1}
EXTRA = {

    A['cache_range_a']: note('cache_op_range_a'), A['cache_range_b']: note('cache_op_range_b'), A['icache_flush']: note('icache_flush_all'),
    A['dcache_flush']: note('dcache_flush_all'), A['dscache_flush']: note('dcache_scache_flush_all'), A['cache_1bfc']: note('cache_fn_1bfc'),

    A['cache_linesz']: quiet(),   # cache line size

    A['cache_size_init']: note("cache_size_init"),
    A['os_lock']: quiet(),   # os lock (scheduler suspend / critical section)
    A['os_unlock']: quiet(),   # os unlock

}

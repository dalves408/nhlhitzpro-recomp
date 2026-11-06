/* Voice sample pages keep the RAM/window meaning chosen on first use after
 * VOICE_ON. Global DMA resolution still handles voice/SGE/notifier structures.
 * Derived from NHL Hitz Pro's protection; retain every ambiguous page rather
 * than a 16-slot cache which can evict a still-playing voice's decision.
 * Same LGPL-2.1-or-later license as apu_vp.c. Caller holds the voice lock. */
#define VP_PAGE_SHIFT 12u
#define VP_PAGE_SIZE (1u << VP_PAGE_SHIFT)
#define VP_AMBIGUOUS_BYTES (64u << 20)
#define VP_CHOICE_BYTES (VP_AMBIGUOUS_BYTES / VP_PAGE_SIZE / 4)
/* 2 bits/page: 0 unknown, 1 RAM, 2 contiguous window. 1 MiB for 256 voices. */
static uint8_t s_vp_page_choice[MCPX_HW_MAX_VOICES][VP_CHOICE_BYTES];
static void vp_pages_forget(uint32_t v)
{
    if(v<MCPX_HW_MAX_VOICES)memset(s_vp_page_choice[v],0,sizeof s_vp_page_choice[v]);
}
static uint8_t *vp_phys(uint32_t v,hwaddr addr)
{
    uint32_t a=(uint32_t)addr&0x0FFFFFFFu;
    if(v>=MCPX_HW_MAX_VOICES||a>=VP_AMBIGUOUS_BYTES)return mcpx_apu_phys(addr);
    uint32_t page=a>>VP_PAGE_SHIFT,shift=(page&3u)*2u;
    uint8_t *slot=&s_vp_page_choice[v][page>>2];
    unsigned choice=(*slot>>shift)&3u;
    if(!choice){
        uintptr_t ram=(uintptr_t)g_apu_ram_ptr+(a&~(VP_PAGE_SIZE-1u));
        uint8_t *base=mcpx_apu_phys((hwaddr)(a&~(VP_PAGE_SIZE-1u)));
        if((uintptr_t)base==ram)choice=1;
        else if((uintptr_t)base==ram+0x80000000ull)choice=2;
        else return mcpx_apu_phys(addr); /* Preserve any future resolver domain. */
        *slot=(uint8_t)((*slot&~(3u<<shift))|(choice<<shift));
    }
    return (uint8_t *)((uintptr_t)g_apu_ram_ptr+a+(choice==2?0x80000000ull:0));
}
static void vp_read(uint32_t v,hwaddr addr,void *output,size_t bytes)
{
    uint8_t *out=output;
    while(bytes){
        size_t n=VP_PAGE_SIZE-((uint32_t)addr&(VP_PAGE_SIZE-1u));if(n>bytes)n=bytes;
        memcpy(out,vp_phys(v,addr),n);out+=n;addr+=n;bytes-=n;
    }
}
static uint8_t vp_ldub(uint32_t v,hwaddr a){return *vp_phys(v,a);}
static uint16_t vp_lduw(uint32_t v,hwaddr a)
{
    uint16_t x;
    if(((uint32_t)a&(VP_PAGE_SIZE-1u))<=VP_PAGE_SIZE-sizeof x)memcpy(&x,vp_phys(v,a),sizeof x);
    else vp_read(v,a,&x,sizeof x);
    return x;
}
static uint32_t vp_ldl(uint32_t v,hwaddr a)
{
    uint32_t x;
    if(((uint32_t)a&(VP_PAGE_SIZE-1u))<=VP_PAGE_SIZE-sizeof x)memcpy(&x,vp_phys(v,a),sizeof x);
    else vp_read(v,a,&x,sizeof x);
    return x;
}

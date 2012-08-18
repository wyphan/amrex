#include <winstd.H>

#include <FabArray.H>
#include <ParmParse.H>
//
// Set default values in Initialize()!!!
//
bool FabArrayBase::verbose;
bool FabArrayBase::do_alltoallv;
bool FabArrayBase::do_async_sends;
bool FabArrayBase::do_not_use_cache;

namespace
{
    bool initialized = false;
    //
    // Set default values in Initialize()!!!
    //
    bool use_copy_cache;
    int  copy_cache_max_size;
    bool use_fb_cache;
    int  fb_cache_max_size;
}

void
FabArrayBase::Initialize ()
{
    if (initialized) return;
    //
    // Set default values here!!!
    //
    FabArrayBase::verbose          = false;
    FabArrayBase::do_alltoallv     = false;
    FabArrayBase::do_async_sends   = false;
    FabArrayBase::do_not_use_cache = false;

    use_copy_cache      = true;
    copy_cache_max_size = 30;   // -1 ==> no maximum size
    use_fb_cache        = true;
    fb_cache_max_size   = 30;   // -1 ==> no maximum size

    ParmParse pp("fabarray");

    pp.query("verbose",          FabArrayBase::verbose);
    pp.query("do_alltoallv",     FabArrayBase::do_alltoallv);
    pp.query("do_async_sends",   FabArrayBase::do_async_sends);
    pp.query("do_not_use_cache", FabArrayBase::do_not_use_cache);

    pp.query("use_copy_cache",      use_copy_cache);
    pp.query("copy_cache_max_size", copy_cache_max_size);
    pp.query("use_fb_cache",        use_fb_cache);
    pp.query("fb_cache_max_size",   fb_cache_max_size);

    if (fb_cache_max_size <= 0 && fb_cache_max_size != -1)
        use_fb_cache = false;

    if (copy_cache_max_size <= 0 && copy_cache_max_size != -1)
        use_copy_cache = false;

    if (do_alltoallv && do_async_sends)
        BoxLib::Abort("At most one of 'do_alltoallv' and 'do_async_sends' can be true");

    BoxLib::ExecOnFinalize(FabArrayBase::Finalize);

    initialized = true;
}

FabArrayBase::FabArrayBase ()
{
    Initialize();
}

FabArrayBase::~FabArrayBase () {}

const Box
FabArrayBase::fabbox (int K) const
{
    return BoxLib::grow(boxarray[K], n_grow);
}

FabArrayBase::FabComTag::FabComTag ()
{
    fromProc          = 0;
    toProc            = 0;
    fabIndex          = 0;
    fineIndex         = 0;
    srcComp           = 0;
    destComp          = 0;
    nComp             = 0;
    face              = 0;
    fabArrayId        = 0;
    fillBoxId         = 0;
    procThatNeedsData = 0;
    procThatHasData   = 0;
}

//
// Used to cache some CommData stuff in CollectData().
//

FabArrayBase::CommDataCache::CommDataCache ()
    :
    m_valid(false)
{}

void
FabArrayBase::CommDataCache::operator= (const Array<ParallelDescriptor::CommData>& rhs)
{
    m_commdata = rhs;
    m_valid    = true;
}

//
// Stuff used for copy() caching.
//

FabArrayBase::CPC::CPC ()
    :
    m_reused(false)
{}

FabArrayBase::CPC::CPC (const BoxArray&            dstba,
                        const BoxArray&            srcba,
                        const DistributionMapping& dstdm,
                        const DistributionMapping& srcdm)
    :
    m_dstba(dstba),
    m_srcba(srcba),
    m_dstdm(dstdm),
    m_srcdm(srcdm),
    m_reused(false)
{}

FabArrayBase::CPC::CPC (const CPC& rhs)
    :
    m_dstba(rhs.m_dstba),
    m_srcba(rhs.m_srcba),
    m_dstdm(rhs.m_dstdm),
    m_srcdm(rhs.m_srcdm),
    m_LocTags(rhs.m_LocTags),
    m_SndTags(rhs.m_SndTags),
    m_RcvTags(rhs.m_RcvTags),
    m_SndVols(rhs.m_SndVols),
    m_RcvVols(rhs.m_RcvVols),
    m_reused(rhs.m_reused)
{}

FabArrayBase::CPC::~CPC () {}

typedef std::multimap<int,FabArrayBase::CPC> CPCCache;

typedef CPCCache::iterator CPCCacheIter;

static CPCCache TheCopyCache;

FabArrayBase::CPC&
FabArrayBase::CPC::TheCPC (const CPC& cpc)
{
    const int key = cpc.m_dstba.size() + cpc.m_srcba.size();

    if (use_copy_cache)
    {
        std::pair<CPCCacheIter,CPCCacheIter> er_it = TheCopyCache.equal_range(key);

        for (CPCCacheIter it = er_it.first; it != er_it.second; ++it)
        {
            if (it->second == cpc)
            {
                it->second.m_reused = true;

                return it->second;
            }
        }

        if (TheCopyCache.size() >= copy_cache_max_size && copy_cache_max_size != -1)
        {
            //
            // Don't let the size of the cache get too big.
            //
            for (CPCCacheIter it = TheCopyCache.begin(); it != TheCopyCache.end(); )
            {
                if (!it->second.m_reused)
                {
                    TheCopyCache.erase(it++);

                    if (TheCopyCache.size() < copy_cache_max_size)
                        //
                        // Only delete enough entries to stay under limit.
                        //
                        break;
                }
                else
                {
                    ++it;
                }
            }

            if (TheCopyCache.size() >= copy_cache_max_size)
            {
                //
                // Get rid of entry with the smallest key.
                //
                TheCopyCache.erase(TheCopyCache.begin());
            }
        }
    }
    else
    {
        TheCopyCache.clear();
    }

    CPCCacheIter it = TheCopyCache.insert(std::make_pair(key,cpc));
    //
    // Got to build it.
    //
    CPC& thecpc = it->second;

    CopyComTag tag;

    std::vector< std::pair<int,Box> > isects;

    const int MyProc = ParallelDescriptor::MyProc();

    for (int i = 0, N = thecpc.m_dstba.size(); i < N; i++)
    {
        thecpc.m_srcba.intersections(thecpc.m_dstba[i],isects);

        const int d_owner = thecpc.m_dstdm[i];

        for (int j = 0, M = isects.size(); j < M; j++)
        {
            const Box& bx      = isects[j].second;
            const int  k       = isects[j].first;
            const int  s_owner = thecpc.m_srcdm[k];

            if (d_owner != MyProc && s_owner != MyProc) continue;

            tag.box = bx;

            const int vol = bx.numPts();

            if (d_owner == MyProc)
            {
                tag.fabIndex = i;

                if (s_owner == MyProc)
                {
                    tag.srcIndex = k;

                    thecpc.m_LocTags.push_back(tag);
                }
                else
                {
                    thecpc.m_RcvTags[s_owner].push_back(tag);

                    if (thecpc.m_RcvVols.count(s_owner) > 0)
                    {
                        thecpc.m_RcvVols[s_owner] += vol;
                    }
                    else
                    {
                        thecpc.m_RcvVols[s_owner] = vol;
                    }
                }
            }
            else if (s_owner == MyProc)
            {
                tag.fabIndex = k;

                thecpc.m_SndTags[d_owner].push_back(tag);

                if (thecpc.m_SndVols.count(d_owner) > 0)
                {
                    thecpc.m_SndVols[d_owner] += vol;
                }
                else
                {
                    thecpc.m_SndVols[d_owner] = vol;
                }
            }
        }
    }

    return thecpc;
}

void
FabArrayBase::CPC::FlushCache ()
{
    if (ParallelDescriptor::IOProcessor() && !TheCopyCache.empty() && FabArrayBase::verbose)
    {
        int reused = 0;

        for (CPCCacheIter it = TheCopyCache.begin(), End = TheCopyCache.end(); it != End; ++it)
            if (it->second.m_reused)
                reused++;

        std::cout << "CPC::TheCopyCache.size() = " << TheCopyCache.size() << ", # reused = " << reused << '\n';
    }
    TheCopyCache.clear();
}

FabArrayBase::SI::~SI () {}

typedef std::multimap<int,FabArrayBase::SI> SIMMap;

typedef SIMMap::iterator SIMMapIter;

static SIMMap SICache;

void
FabArrayBase::Finalize ()
{
    SICache.clear();

    TheCopyCache.clear();

    initialized = false;
}

void
FabArrayBase::FlushSICache ()
{
    if (ParallelDescriptor::IOProcessor() && !SICache.empty() && FabArrayBase::verbose)
    {
        int reused = 0;

        for (SIMMapIter it = SICache.begin(), End = SICache.end(); it != End; ++it)
            if (it->second.m_reused)
                reused++;

        std::cout << "FabArrayBase::SICache.size() = " << SICache.size() << ", # reused = " << reused << '\n';
    }

    SICache.clear();
}

int
FabArrayBase::SICacheSize ()
{
    return SICache.size();
}

FabArrayBase::SI&
FabArrayBase::TheFBsirec (bool                cross,
                          const FabArrayBase& mf)
{
    const FabArrayBase::SI si(mf.boxArray(), mf.DistributionMap(), mf.nGrow(), cross);

    const int Key = mf.size() + mf.nGrow() + cross;

    if (use_fb_cache)
    {
        std::pair<SIMMapIter,SIMMapIter> er_it = SICache.equal_range(Key);
    
        for (SIMMapIter it = er_it.first; it != er_it.second; ++it)
        {
            if (it->second == si)
            {
                it->second.m_reused = true;

                return it->second;
            }
        }

        if (SICache.size() >= fb_cache_max_size && fb_cache_max_size != -1)
        {
            //
            // Don't let the size of the cache get too big.
            //
            for (SIMMapIter it = SICache.begin(); it != SICache.end(); )
            {
                if (!it->second.m_reused)
                {
                    SICache.erase(it++);

                    if (SICache.size() < fb_cache_max_size)
                        //
                        // Only delete enough entries to stay under limit.
                        //
                        break;
                }
                else
                {
                    ++it;
                }
            }

            if (SICache.size() >= fb_cache_max_size)
            {
                //
                // Get rid of entry with the smallest key.
                //
                SICache.erase(SICache.begin());
            }
        }
    }
    else
    {
        SICache.clear();
    }
    //
    // Got to build one.
    //
    SIMMapIter                 it     = SICache.insert(std::make_pair(Key,si));
    const BoxArray&            ba     = mf.boxArray();
    const DistributionMapping& dm     = mf.DistributionMap();
    const int                  MyProc = ParallelDescriptor::MyProc();
    SI&                        TheSI  = it->second;

    CopyComTag                        tag;
    std::vector<Box>                  boxes;
    std::vector< std::pair<int,Box> > isects;

    boxes.resize(si.m_cross ? 2*BL_SPACEDIM : 1);

    for (int i = 0, N = ba.size(); i < N; i++)
    {
        if (si.m_cross)
        {
            const Box& vbx = ba[i];

            for (int dir = 0; dir < BL_SPACEDIM; dir++)
            {
                Box lo = vbx;
                lo.setSmall(dir, vbx.smallEnd(dir) - si.m_ngrow);
                lo.setBig  (dir, vbx.smallEnd(dir) - 1);
                boxes[2*dir+0] = lo;

                Box hi = vbx;
                hi.setSmall(dir, vbx.bigEnd(dir) + 1);
                hi.setBig  (dir, vbx.bigEnd(dir) + si.m_ngrow);
                boxes[2*dir+1] = hi;
            }
        }
        else
        {
            boxes[0] = BoxLib::grow(ba[i],si.m_ngrow);
        }

        const int d_owner = dm[i];

        for (std::vector<Box>::const_iterator it = boxes.begin(),
                 End = boxes.end();
             it != End;
             ++it)
        {
            ba.intersections(*it,isects);

            for (int j = 0, M = isects.size(); j < M; j++)
            {
                const Box& bx      = isects[j].second;
                const int  k       = isects[j].first;
                const int  s_owner = dm[k];

                if (k == i) continue;

                if (d_owner != MyProc && s_owner != MyProc) continue;

                const int vol = bx.numPts();

                tag.box = bx;

                if (d_owner == MyProc)
                {
                    tag.fabIndex = i;

                    if (s_owner == MyProc)
                    {
                        tag.srcIndex = k;

                        TheSI.m_LocTags.push_back(tag);
                    }
                    else
                    {
                        TheSI.m_RcvTags[s_owner].push_back(tag);

                        if (TheSI.m_RcvVols.count(s_owner) > 0)
                        {
                            TheSI.m_RcvVols[s_owner] += vol;
                        }
                        else
                        {
                            TheSI.m_RcvVols[s_owner] = vol;
                        }
                    }
                }
                else if (s_owner == MyProc)
                {
                    tag.fabIndex = k;

                    TheSI.m_SndTags[d_owner].push_back(tag);

                    if (TheSI.m_SndVols.count(d_owner) > 0)
                    {
                        TheSI.m_SndVols[d_owner] += vol;
                    }
                    else
                    {
                        TheSI.m_SndVols[d_owner] = vol;
                    }
                }
            }
        }
    }

    return TheSI;
}

#include "MDParticleContainer.H"
#include "Constants.H"

#include "CheckPair.H"

using namespace amrex;

namespace
{    
    void get_position_unit_cell(Real* r, const IntVect& nppc, int i_part)
    {
        int nx = nppc[0];
        int ny = nppc[1];
        int nz = nppc[2];
        
        int ix_part = i_part/(ny * nz);
        int iy_part = (i_part % (ny * nz)) % ny;
        int iz_part = (i_part % (ny * nz)) / ny;
        
        r[0] = (0.5+ix_part)/nx;
        r[1] = (0.5+iy_part)/ny;
        r[2] = (0.5+iz_part)/nz;
    }
    
    void get_gaussian_random_momentum(Real* u, Real u_mean, Real u_std) {
        Real ux_th = amrex::RandomNormal(0.0, u_std);
        Real uy_th = amrex::RandomNormal(0.0, u_std);
        Real uz_th = amrex::RandomNormal(0.0, u_std);
        
        u[0] = u_mean + ux_th;
        u[1] = u_mean + uy_th;
        u[2] = u_mean + uz_th;
    }
    
    Vector<Box> getBoundaryBoxes(const Box& box, const int ncells)
    {            
        AMREX_ASSERT_WITH_MESSAGE(box.size() > 2*IntVect(AMREX_D_DECL(ncells, ncells, ncells)),
                                  "Too many cells requested in getBoundaryBoxes");
        
        AMREX_ASSERT_WITH_MESSAGE(box.ixType().cellCentered(), 
                                  "Box must be cell-centered");
        
        Vector<Box> bl;
        for (int i = 0; i < AMREX_SPACEDIM; ++i) {
            BoxList face_boxes;
            Box hi_face_box = adjCellHi(box, i, ncells);
            Box lo_face_box = adjCellLo(box, i, ncells);
            face_boxes.push_back(hi_face_box); bl.push_back(hi_face_box);
            face_boxes.push_back(lo_face_box); bl.push_back(lo_face_box);
            for (auto face_box : face_boxes) {
                for (int j = 0; j < AMREX_SPACEDIM; ++j) {
                    if (i == j) continue;
                    BoxList edge_boxes;
                    Box hi_edge_box = adjCellHi(face_box, j, ncells);
                    Box lo_edge_box = adjCellLo(face_box, j, ncells);
                    edge_boxes.push_back(hi_edge_box); bl.push_back(hi_edge_box);
                    edge_boxes.push_back(lo_edge_box); bl.push_back(lo_edge_box);
                    for (auto edge_box : edge_boxes) {                    
                        for (int k = 0; k < AMREX_SPACEDIM; ++k) {
                            if ((j == k) or (i == k)) continue;
                            Box hi_corner_box = adjCellHi(edge_box, k, ncells);
                            Box lo_corner_box = adjCellLo(edge_box, k, ncells);
                            bl.push_back(hi_corner_box);
                            bl.push_back(lo_corner_box);
                        }
                    }
                }
            }
        }
        
        RemoveDuplicates(bl);
        return bl;
    }
}

MDParticleContainer::
MDParticleContainer(const Geometry            & a_geom,
                    const DistributionMapping & a_dmap,
                    const BoxArray            & a_ba)
    : ParticleContainer<PIdx::ncomps>(a_geom, a_dmap, a_ba)
{
    BL_PROFILE("MDParticleContainer::MDParticleContainer");

    buildNeighborMask();
}

void
MDParticleContainer::
buildNeighborMask()
{    
    BL_PROFILE("MDParticleContainer::buildNeighborMask");

    m_neighbor_mask_initialized = true;

    const int lev = 0;
    const Geometry& geom = this->Geom(lev);
    const BoxArray& ba = this->ParticleBoxArray(lev);
    const DistributionMapping& dmap = this->ParticleDistributionMap(lev);

    m_neighbor_mask_ptr.reset(new iMultiFab(ba, dmap, 1, 0));
    m_neighbor_mask_ptr->setVal(-1);

    const Periodicity& periodicity = geom.periodicity();
    const std::vector<IntVect>& pshifts = periodicity.shiftIntVect();

    for (MFIter mfi(ba, dmap); mfi.isValid(); ++mfi)
    {
        int grid = mfi.index();

        std::set<std::pair<int, Box> > neighbor_grids;
        for (auto pit=pshifts.cbegin(); pit!=pshifts.cend(); ++pit)
        {
            const Box box = ba[mfi] + *pit;

            const bool first_only = false;
            auto isecs = ba.intersections(box, first_only, m_ncells);
        
            for (auto& isec : isecs)
            {
                int nbor_grid = isec.first;
                const Box isec_box = isec.second - *pit;
                if (grid != nbor_grid) neighbor_grids.insert(std::make_pair(nbor_grid, isec_box));
            }
        }
        
        BoxList isec_bl;
        std::vector<int> isec_grids;
        for (auto nbor_grid : neighbor_grids)
        {
            isec_grids.push_back(nbor_grid.first);
            isec_bl.push_back(nbor_grid.second);
        }
        BoxArray isec_ba(isec_bl);
        
        Vector<Box> bl = getBoundaryBoxes(amrex::grow(ba[mfi], -m_ncells), m_ncells);
        
        m_grid_map[grid].resize(bl.size());
        for (int i = 0; i < static_cast<int>(bl.size()); ++i)
        {
            const Box& box = bl[i];
            
            const int nGrow = 0;
            const bool first_only = false;
            auto isecs = isec_ba.intersections(box, first_only, nGrow);

            if (! isecs.empty() ) (*m_neighbor_mask_ptr)[mfi].setVal(i, box);

            for (auto& isec : isecs)
            {
                m_grid_map[grid][i].push_back(isec_grids[isec.first]);
            }
        }
    }
}

void 
MDParticleContainer::
sortParticlesByNeighborDest()
{
    BL_PROFILE("MDParticleContainer::sortParticlesByNeighborDest");

    const int lev = 0;
    const auto& geom = Geom(lev);
    const auto dxi = Geom(lev).InvCellSizeArray();
    const auto plo = Geom(lev).ProbLoArray();
    const auto domain = Geom(lev).Domain();
    auto& plev  = GetParticles(lev);
    auto& ba = this->ParticleBoxArray(lev);
    auto& dmap = this->ParticleDistributionMap(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();        
        auto index = std::make_pair(gid, tid);

        const Box& bx = mfi.tilebox();
        const auto lo = amrex::lbound(bx);
        const auto hi = amrex::ubound(bx);

        auto& src_tile = plev[index];
        auto& aos   = src_tile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        Gpu::ManagedDeviceVector<int> neighbor_codes(np);
        int* pcodes = neighbor_codes.dataPtr();

        BaseFab<int>* mask_ptr = m_neighbor_mask_ptr->fabPtr(mfi);
        
        ParticleType* p_ptr = &(aos[0]);
        AMREX_FOR_1D ( np, i,
        {
	    IntVect iv = IntVect(
                AMREX_D_DECL(floor((p_ptr[i].pos(0)-plo[0])*dxi[0]),
                             floor((p_ptr[i].pos(1)-plo[1])*dxi[1]),
                             floor((p_ptr[i].pos(2)-plo[2])*dxi[2]))
                );
            
            iv += domain.smallEnd();
            
	    int code = (*mask_ptr)(iv);
            
            pcodes[i] = code;
        });

        thrust::sort_by_key(thrust::cuda::par(Cuda::The_ThrustCachedAllocator()),
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            aos().begin());

        int num_codes = m_grid_map[gid].size();
        Gpu::ManagedDeviceVector<int> neighbor_code_begin(num_codes + 1);
        Gpu::ManagedDeviceVector<int> neighbor_code_end  (num_codes + 1);
        
        thrust::counting_iterator<int> search_begin(-1);
        thrust::lower_bound(thrust::device,
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            search_begin,
                            search_begin + num_codes + 1,
                            neighbor_code_begin.begin());
        
        thrust::upper_bound(thrust::device,
                            neighbor_codes.begin(),
                            neighbor_codes.end(),
                            search_begin,
                            search_begin + num_codes + 1,
                            neighbor_code_end.begin());
        
        m_start[gid].resize(num_codes + 1);
        m_stop[gid].resize(num_codes + 1);
        
        Cuda::thrust_copy(neighbor_code_begin.begin(),
                          neighbor_code_begin.end(),
                          m_start[gid].begin());
        
        Cuda::thrust_copy(neighbor_code_end.begin(),
                          neighbor_code_end.end(),
                          m_stop[gid].begin());
    }
}

void
MDParticleContainer::
RedistributeLocal()
{
    const int lev_min = 0;
    const int lev_max = 0;
    const int nGrow = 0;
    const int local = 1;
    clearNeighbors();
    Redistribute(lev_min, lev_max, nGrow, local);    
}

void
MDParticleContainer::
fillNeighbors()
{
    BL_PROFILE("MDParticleContainer::fillNeighbors");

    sortParticlesByNeighborDest();
    updateNeighbors();
}

void
MDParticleContainer::
updateNeighbors()
{
    BL_PROFILE("MDParticleContainer::updateNeighbors");

    clearNeighbors();

    const int lev = 0;
    auto& plev  = GetParticles(lev);
    auto& ba = this->ParticleBoxArray(lev);
    auto& dmap = this->ParticleDistributionMap(lev);
    
    std::map<int, SendBuffer> not_ours;
    std::map<int, size_t> grid_counts;
    
    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int src_grid = mfi.index();
        int num_codes = m_grid_map[src_grid].size();
        for (int i = 1; i < num_codes + 1; ++i)
        {
            for (auto dst_grid : m_grid_map[src_grid][i-1])
            {            
                const int dest_proc = dmap[dst_grid];
                if (dest_proc != ParallelDescriptor::MyProc())
                {
                    grid_counts[dest_proc] += 1;
                }
            }
        }
    }

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int src_grid = mfi.index();
        int src_tile = mfi.LocalTileIndex();
        AMREX_ASSERT(src_tile == 0);
        auto index = std::make_pair(src_grid, src_tile);

        const Box& bx = mfi.tilebox();
        const auto lo = amrex::lbound(bx);
        const auto hi = amrex::ubound(bx);

        auto& src_ptile = plev[index];

        int num_codes = m_grid_map[src_grid].size();
        for (int i = 1; i < num_codes + 1; ++i)
        {
            const size_t num_to_add = m_stop[src_grid][i] - m_start[src_grid][i];
            for (auto dst_grid : m_grid_map[src_grid][i-1]) {
                const int dst_tile = 0;                
                auto pair_index = std::make_pair(dst_grid, dst_tile);
                const int dest_proc = dmap[dst_grid];
                if (dest_proc == ParallelDescriptor::MyProc())  // this is a local copy
                {
                    auto& dst_ptile = plev[pair_index];
                    const int nRParticles = dst_ptile.numRealParticles();
                    const int nNParticles = dst_ptile.getNumNeighbors();
                    const int new_num_neighbors = nNParticles + num_to_add;
                    dst_ptile.setNumNeighbors(new_num_neighbors);
                    
                    // copy structs
                    {
                        auto& src = src_ptile.GetArrayOfStructs();
                        auto& dst = dst_ptile.GetArrayOfStructs();
                        thrust::copy(thrust::device,
                                     src.begin() + m_start[src_grid][i], src.begin() + m_stop[src_grid][i], 
                                     dst().begin() + nRParticles + nNParticles);
                    }
                }
                else { // this is the non-local case
                    char* dst;
                    const size_t old_size = not_ours[dest_proc].size();
                    const size_t new_size
                        = old_size + num_to_add*sizeof(ParticleType) + sizeof(size_t) + 2*sizeof(int);
                
                    if (old_size == 0)
                    {
                        not_ours[dest_proc].resize(new_size + sizeof(size_t));
                        cudaMemcpyAsync(thrust::raw_pointer_cast(not_ours[dest_proc].data()),
                                        &grid_counts[dest_proc], sizeof(size_t), cudaMemcpyHostToHost);
                        dst = thrust::raw_pointer_cast(
                            not_ours[dest_proc].data() + old_size + sizeof(size_t));
                    } else
                    {
                        not_ours[dest_proc].resize(new_size);
                        dst = thrust::raw_pointer_cast(not_ours[dest_proc].data() + old_size);
                    }
                    
                    cudaMemcpyAsync(thrust::raw_pointer_cast(dst), 
                                    &num_to_add, sizeof(size_t), cudaMemcpyHostToHost);
                    dst += sizeof(size_t);
                    
                    cudaMemcpyAsync(thrust::raw_pointer_cast(dst), &dst_grid, sizeof(int), 
                                    cudaMemcpyHostToHost);
                    dst += sizeof(int);
                    
                    cudaMemcpyAsync(thrust::raw_pointer_cast(dst), 
                                    &dest_proc, sizeof(int), cudaMemcpyHostToHost);
                    dst += sizeof(int);
                    
                    // pack structs
                    {
                        auto& aos = src_ptile.GetArrayOfStructs();
                        cudaMemcpyAsync(thrust::raw_pointer_cast(dst), 
                                        thrust::raw_pointer_cast(aos().data() + m_start[src_grid][i]),
                                        num_to_add*sizeof(ParticleType), cudaMemcpyDeviceToHost);
                        dst += num_to_add*sizeof(ParticleType);
                    }
                }
            }
        }
    }

    if (ParallelDescriptor::NProcs() == 1) {
        BL_ASSERT(not_ours.empty());
    }
    else {
        fillNeighborsMPIGPU(not_ours);
    }
}

void
MDParticleContainer
::fillNeighborsMPIGPU (std::map<int, SendBuffer>& not_ours)
{
    BL_PROFILE("ParticleContainer::fillNeighborsMPIGPU()");

#if BL_USE_MPI
    const int NProcs = ParallelDescriptor::NProcs();
    const int lev = 0;
    
    // We may now have particles that are rightfully owned by another CPU.
    Vector<long> Snds(NProcs, 0), Rcvs(NProcs, 0);  // bytes!

    long NumSnds = 0;

    for (const auto& kv : not_ours)
    {
        const size_t nbytes = kv.second.size();
        Snds[kv.first] = nbytes;
        NumSnds += nbytes;
    }
    
    ParallelDescriptor::ReduceLongMax(NumSnds);

    if (NumSnds == 0) return;

    BL_COMM_PROFILE(BLProfiler::Alltoall, sizeof(long),
                    ParallelDescriptor::MyProc(), BLProfiler::BeforeCall());
    
    BL_MPI_REQUIRE( MPI_Alltoall(Snds.dataPtr(),
                                 1,
                                 ParallelDescriptor::Mpi_typemap<long>::type(),
                                 Rcvs.dataPtr(),
                                 1,
                                 ParallelDescriptor::Mpi_typemap<long>::type(),
                                 ParallelDescriptor::Communicator()) );
    
    BL_ASSERT(Rcvs[ParallelDescriptor::MyProc()] == 0);
    
    BL_COMM_PROFILE(BLProfiler::Alltoall, sizeof(long),
                    ParallelDescriptor::MyProc(), BLProfiler::AfterCall());

    Vector<int> RcvProc;
    Vector<std::size_t> rOffset; // Offset (in bytes) in the receive buffer
    
    std::size_t TotRcvBytes = 0;
    for (int i = 0; i < NProcs; ++i) {
        if (Rcvs[i] > 0) {
            RcvProc.push_back(i);
            rOffset.push_back(TotRcvBytes);
            TotRcvBytes += Rcvs[i];
        }
    }
    
    const int nrcvs = RcvProc.size();
    Vector<MPI_Status>  stats(nrcvs);
    Vector<MPI_Request> rreqs(nrcvs);

    const int SeqNum = ParallelDescriptor::SeqNum();
    
    // Allocate data for rcvs as one big chunk.    
    char* rcv_buffer;
    if (ParallelDescriptor::UseGpuAwareMpi()) 
    {
        rcv_buffer = static_cast<char*>(amrex::The_Device_Arena()->alloc(TotRcvBytes));
    }
    else 
    {
        rcv_buffer = static_cast<char*>(amrex::The_Pinned_Arena()->alloc(TotRcvBytes));
    }
    
    // Post receives.
    for (int i = 0; i < nrcvs; ++i) {
        const auto Who    = RcvProc[i];
        const auto offset = rOffset[i];
        const auto Cnt    = Rcvs[Who];
        
        BL_ASSERT(Cnt > 0);
        BL_ASSERT(Cnt < std::numeric_limits<int>::max());
        BL_ASSERT(Who >= 0 && Who < NProcs);
        
        rreqs[i] = ParallelDescriptor::Arecv(rcv_buffer + offset,
                                             Cnt, Who, SeqNum).req();
    }
    
    // Send.
    for (const auto& kv : not_ours) {
        const auto Who = kv.first;
        const auto Cnt = kv.second.size();

        BL_ASSERT(Cnt > 0);
        BL_ASSERT(Who >= 0 && Who < NProcs);
        BL_ASSERT(Cnt < std::numeric_limits<int>::max());
        
        ParallelDescriptor::Send(thrust::raw_pointer_cast(kv.second.data()),
                                 Cnt, Who, SeqNum);
    }

    if (nrcvs > 0) {
        ParallelDescriptor::Waitall(rreqs, stats);

        for (int i = 0; i < nrcvs; ++i) {
            const int offset = rOffset[i];
            char* buffer = thrust::raw_pointer_cast(rcv_buffer + offset);
            size_t num_grids, num_particles;
            int gid, pid;
            cudaMemcpy(&num_grids, buffer, sizeof(size_t), cudaMemcpyHostToHost);
            buffer += sizeof(size_t);

            for (int g = 0; g < num_grids; ++g) {
                cudaMemcpyAsync(&num_particles, buffer, sizeof(size_t), cudaMemcpyHostToHost);
                buffer += sizeof(size_t);
                cudaMemcpyAsync(&gid, buffer, sizeof(int), cudaMemcpyHostToHost);
                buffer += sizeof(int);
                cudaMemcpyAsync(&pid, buffer, sizeof(int), cudaMemcpyHostToHost);
                buffer += sizeof(int);

                Gpu::Device::streamSynchronize();

                if (num_particles == 0) continue;

                AMREX_ALWAYS_ASSERT(pid == ParallelDescriptor::MyProc());
                {
                    const int tid = 0;
                    auto pair_index = std::make_pair(gid, tid);
                    auto& ptile = GetParticles(lev)[pair_index];
                    const int nRParticles = ptile.numRealParticles();
                    const int nNParticles = ptile.getNumNeighbors();
                    const int new_num_neighbors = nNParticles + num_particles;
                    ptile.setNumNeighbors(new_num_neighbors);

                    //copy structs
                    auto& aos = ptile.GetArrayOfStructs();
                    cudaMemcpyAsync(static_cast<ParticleType*>(aos().data()) + nRParticles + nNParticles,
                                    buffer, num_particles*sizeof(ParticleType),
                                    cudaMemcpyHostToDevice);
                    buffer += num_particles*sizeof(ParticleType);
                }
            }
        }
    }

    if (ParallelDescriptor::UseGpuAwareMpi())
    {
        amrex::The_Device_Arena()->free(rcv_buffer);
    } else {
        amrex::The_Pinned_Arena()->free(rcv_buffer);
    }

#endif // MPI    
}

void
MDParticleContainer::
clearNeighbors()
{
    BL_PROFILE("MDParticleContainer::clearNeighbors");

    const int lev = 0;

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int src_grid = mfi.index();
        int src_tile = mfi.LocalTileIndex();
        AMREX_ASSERT(src_tile == 0);
        auto index = std::make_pair(src_grid, src_tile);
        auto& ptile = GetParticles(lev)[index];
        ptile.setNumNeighbors(0);
    }
}

void
MDParticleContainer::
InitParticles(const IntVect& a_num_particles_per_cell,
              const Real     a_thermal_momentum_std,
              const Real     a_thermal_momentum_mean)
{
    BL_PROFILE("MDParticleContainer::InitParticles");

    amrex::Print() << "Generating particles... ";

    const int lev = 0;   
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    
    const int num_ppc = AMREX_D_TERM( a_num_particles_per_cell[0],
                                     *a_num_particles_per_cell[1],
                                     *a_num_particles_per_cell[2]);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();

        Cuda::HostVector<ParticleType> host_particles;
        
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv)) {
            for (int i_part=0; i_part<num_ppc;i_part++) {
                Real r[3];
                Real v[3];
                
                get_position_unit_cell(r, a_num_particles_per_cell, i_part);
                
                get_gaussian_random_momentum(v, a_thermal_momentum_mean,
                                             a_thermal_momentum_std);
                
                Real x = plo[0] + (iv[0] + r[0])*dx[0];
                Real y = plo[1] + (iv[1] + r[1])*dx[1];
                Real z = plo[2] + (iv[2] + r[2])*dx[2];
                
                ParticleType p;
                p.id()  = ParticleType::NextID();
                p.cpu() = ParallelDescriptor::MyProc();                
                p.pos(0) = x;
                p.pos(1) = y;
                p.pos(2) = z;
                
                p.rdata(PIdx::vx) = v[0];
                p.rdata(PIdx::vy) = v[1];
                p.rdata(PIdx::vz) = v[2];

                p.rdata(PIdx::ax) = 0.0;
                p.rdata(PIdx::ay) = 0.0;
                p.rdata(PIdx::az) = 0.0;
                
                host_particles.push_back(p);
            }
        }
        
        auto& particles = GetParticles(lev);
        auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];
        auto old_size = particle_tile.GetArrayOfStructs().size();
        auto new_size = old_size + host_particles.size();
        particle_tile.resize(new_size);
        
        Cuda::thrust_copy(host_particles.begin(),
                          host_particles.end(),
                          particle_tile.GetArrayOfStructs().begin() + old_size);        
    }

    amrex::Print() << "done. \n";
}

void MDParticleContainer::BuildNeighborList()
{
    BL_PROFILE("MDParticleContainer::BuildNeighborList");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();        
        auto index = std::make_pair(gid, tid);

        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();

        Box bx = mfi.tilebox();
        bx.grow(m_ncells);

        m_neighbor_list[index].build(aos(), bx, geom, CheckPair());
    }
}

void MDParticleContainer::printNeighborList()
{
    BL_PROFILE("MDParticleContainer::printNeighborList");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        auto index = std::make_pair(gid, tid);

        m_neighbor_list[index].print();
    }
}

void MDParticleContainer::computeForces()
{
    BL_PROFILE("MDParticleContainer::computeForces");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        auto index = std::make_pair(gid, tid);

        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();
        const size_t np = aos.numParticles();

        auto nbor_data = m_neighbor_list[index].data();
        ParticleType* pstruct = aos().dataPtr();

       // now we loop over the neighbor list and compute the forces
        AMREX_FOR_1D ( np, i,
        {
            ParticleType& p1 = pstruct[i];
            p1.rdata(PIdx::ax) = 0.0;
            p1.rdata(PIdx::ay) = 0.0;
            p1.rdata(PIdx::az) = 0.0;

            for (const auto& p2 : nbor_data.getNeighbors(i))
            {                
                Real dx = p1.pos(0) - p2.pos(0);
                Real dy = p1.pos(1) - p2.pos(1);
                Real dz = p1.pos(2) - p2.pos(2);
                
                Real r2 = dx*dx + dy*dy + dz*dz;
                r2 = amrex::max(r2, Params::min_r*Params::min_r);
                Real r = sqrt(r2);
                
                Real coef = (1.0 - Params::cutoff / r) / r2 / Params::mass;
                p1.rdata(PIdx::ax) += coef * dx;
                p1.rdata(PIdx::ay) += coef * dy;
                p1.rdata(PIdx::az) += coef * dz;
            }
        });
    }
}

void MDParticleContainer::moveParticles(const amrex::Real& dt)
{
    BL_PROFILE("MDParticleContainer::moveParticles");

    const int lev = 0;
    const Geometry& geom = Geom(lev);
    const auto plo = Geom(lev).ProbLoArray();
    const auto phi = Geom(lev).ProbHiArray();
    const auto dx = Geom(lev).CellSizeArray();
    auto& plev  = GetParticles(lev);

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        int gid = mfi.index();
        int tid = mfi.LocalTileIndex();
        
        auto& ptile = plev[std::make_pair(gid, tid)];
        auto& aos   = ptile.GetArrayOfStructs();
        ParticleType* pstruct = &(aos[0]);

        const size_t np = aos.numParticles();
    
        // now we move the particles
        AMREX_FOR_1D ( np, i,
        {
            ParticleType& p = pstruct[i];            
            p.rdata(PIdx::vx) += p.rdata(PIdx::ax) * dt;
            p.rdata(PIdx::vy) += p.rdata(PIdx::ay) * dt;
            p.rdata(PIdx::vz) += p.rdata(PIdx::az) * dt;

            p.pos(0) += p.rdata(PIdx::vx) * dt;
            p.pos(1) += p.rdata(PIdx::vy) * dt;
            p.pos(2) += p.rdata(PIdx::vz) * dt;

            for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) {
                while ( (p.pos(idim) < plo[idim]) or (p.pos(idim) > phi[idim]) ) {
                    if ( p.pos(idim) < plo[idim] ) {
                        p.pos(idim) = 2*plo[idim] - p.pos(idim);
                    } else {
                        p.pos(idim) = 2*phi[idim] - p.pos(idim);
                    }
                    p.rdata(idim) *= -1; // flip velocity
                }
            }
        });
    }
}

void MDParticleContainer::writeParticles(const int n)
{
    BL_PROFILE("MDParticleContainer::writeParticles");
    const std::string& pltfile = amrex::Concatenate("particles", n, 5);
    WriteAsciiFile(pltfile);
}

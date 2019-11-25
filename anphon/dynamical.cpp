/*
 dynamical.cpp

 Copyright (c) 2014, 2015, 2016 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#include "mpi_common.h"
#include "constants.h"
#include "dynamical.h"
#include "error.h"
#include "ewald.h"
#include "system.h"
#include "memory.h"
#include "kpoint.h"
#include "timer.h"
#include "symmetry_core.h"
#include "mathfunctions.h"
#include "fcs_phonon.h"
#include "write_phonons.h"
#include <complex>
#include <vector>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>


using namespace PHON_NS;

Dynamical::Dynamical(PHON *phon): Pointers(phon)
{
    set_default_variables();
}

Dynamical::~Dynamical()
{
    deallocate_variables();
}

void Dynamical::set_default_variables()
{
    neval = 0;
    eigenvectors = true;
    print_eigenvectors = false;
    symmetrize_borncharge = 0;
    nonanalytic = 0;
    participation_ratio = false;
    band_connection = 0;
    na_sigma = 0.0;
    file_born = "";
    UPLO = 'U';

    eval_phonon = nullptr;
    evec_phonon = nullptr;
    index_bconnect = nullptr;
    borncharge = nullptr;

    is_imaginary = nullptr;

    xshift_s = nullptr;
    dymat = nullptr;
    mindist_list = nullptr;
}

void Dynamical::deallocate_variables()
{
    if (eval_phonon) {
        memory->deallocate(eval_phonon);
    }
    if (evec_phonon) {
        memory->deallocate(evec_phonon);
    }
    if (index_bconnect) {
        memory->deallocate(index_bconnect);
    }
    if (borncharge) {
        memory->deallocate(borncharge);
    }
    if (is_imaginary) {
        memory->deallocate(is_imaginary);
    }
    if (xshift_s) {
        memory->deallocate(xshift_s);
    }
    if (dymat) {
        memory->deallocate(dymat);
    }
    if (mindist_list) {
        memory->deallocate(mindist_list);
    }
}


void Dynamical::setup_dynamical(std::string mode)
{
    neval = 3 * system->natmin;

    if (mympi->my_rank == 0) {
        std::cout << std::endl;
        std::cout << " Dynamical matrix" << std::endl;
        std::cout << " ================" << std::endl;
        if (nonanalytic == 0) {
            std::cout << std::endl;
            std::cout << "  NONANALYTIC = 0 : No non-analytic correction. " << std::endl;
            std::cout << std::endl;
        } else if (nonanalytic == 1) {
            std::cout << std::endl;
            std::cout << "  NONANALYTIC = 1 : Non-analytic part of the dynamical matrix will be included " << std::endl;
            std::cout << "                    by the Parlinski's method." << std::endl;
            std::cout << "                    The damping factor for the non-analytic term : " << na_sigma << std::endl;
            std::cout << std::endl;
        } else if (nonanalytic == 2) {
            std::cout << std::endl;
            std::cout << "  NONANALYTIC = 2 : Non-analytic part of the dynamical matrix will be included " << std::endl;
            std::cout << "                    by the mixed-space approach." << std::endl;
            std::cout << std::endl;
        } else if (nonanalytic == 3) {
            std::cout << std::endl;
            std::cout << "  NONANALYTIC = 3 : Non-analytic part of the dynamical matrix will be included " << std::endl;
            std::cout << "                    by the Ewald method." << std::endl;
            std::cout << std::endl;
        }
    }

    memory->allocate(xshift_s, 27, 3);

    for (int i = 0; i < 3; ++i) xshift_s[0][i] = 0.0;
    int icell = 0;

    for (int ix = -1; ix <= 1; ++ix) {
        for (int iy = -1; iy <= 1; ++iy) {
            for (int iz = -1; iz <= 1; ++iz) {
                if (ix == 0 && iy == 0 && iz == 0) continue;

                ++icell;

                xshift_s[icell][0] = static_cast<double>(ix);
                xshift_s[icell][1] = static_cast<double>(iy);
                xshift_s[icell][2] = static_cast<double>(iz);
            }
        }
    }

    if (mympi->my_rank == 0) eigenvectors = true;

    MPI_Bcast(&eigenvectors, 1, MPI_LOGICAL, 0, MPI_COMM_WORLD);
    MPI_Bcast(&nonanalytic, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);
    MPI_Bcast(&band_connection, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);

    if (nonanalytic) {
        setup_dielectric();
        
        MPI_Bcast(&na_sigma, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);

        memory->allocate(mindist_list, system->natmin, system->nat);
        prepare_mindist_list(mindist_list);
    }

    if (mympi->my_rank == 0) {
        std::cout << std::endl;
        std::cout << " -----------------------------------------------------------------"
            << std::endl << std::endl;
    }
}


void Dynamical::prepare_mindist_list(std::vector<int> **mindist_out) const
{
    unsigned int i, j;
    unsigned int nneib = 27;

    double ***xcrd;

    auto nat = system->nat;
    auto natmin = system->natmin;

    std::vector<DistWithCell> **distall;

    memory->allocate(distall, natmin, nat);
    memory->allocate(xcrd, nneib, nat, 3);

    for (i = 0; i < nat; ++i) {
        for (j = 0; j < 3; ++j) {
            xcrd[0][i][j] = system->xr_s[i][j];
        }
    }
    auto icell = 0;
    for (int isize = -1; isize <= 1; ++isize) {
        for (int jsize = -1; jsize <= 1; ++jsize) {
            for (int ksize = -1; ksize <= 1; ++ksize) {

                if (isize == 0 && jsize == 0 && ksize == 0) continue;

                ++icell;
                for (i = 0; i < nat; ++i) {
                    xcrd[icell][i][0] = system->xr_s[i][0] + static_cast<double>(isize);
                    xcrd[icell][i][1] = system->xr_s[i][1] + static_cast<double>(jsize);
                    xcrd[icell][i][2] = system->xr_s[i][2] + static_cast<double>(ksize);
                }
            }
        }
    }

    for (icell = 0; icell < nneib; ++icell) {
        for (i = 0; i < nat; ++i) {
            rotvec(xcrd[icell][i], xcrd[icell][i], system->lavec_s);
        }
    }

    for (i = 0; i < natmin; ++i) {
        unsigned int iat = system->map_p2s[i][0];
        for (j = 0; j < nat; ++j) {
            distall[i][j].clear();
            for (icell = 0; icell < nneib; ++icell) {

                double dist_tmp = distance(xcrd[0][iat], xcrd[icell][j]);
                distall[i][j].emplace_back(icell, dist_tmp);
            }
            std::sort(distall[i][j].begin(), distall[i][j].end());
        }
    }

    // Construct pairs of minimum distance.

    for (i = 0; i < natmin; ++i) {
        for (j = 0; j < nat; ++j) {
            mindist_out[i][j].clear();

            double dist_min = distall[i][j][0].dist;
            for (auto it = distall[i][j].begin(); it != distall[i][j].end(); ++it) {
                if (std::abs((*it).dist - dist_min) < 1.0e-3) {
                    mindist_out[i][j].push_back((*it).cell);
                }
            }
        }
    }

    memory->deallocate(distall);
    memory->deallocate(xcrd);
}


double Dynamical::distance(double *x1,
                           double *x2) const
{
    return std::sqrt(std::pow(x1[0] - x2[0], 2)
        + std::pow(x1[1] - x2[1], 2)
        + std::pow(x1[2] - x2[2], 2));
}


void Dynamical::eval_k(double *xk_in,
                       double *kvec_in,
                       std::vector<FcsClassExtent> fc2_ext,
                       double *eval_out,
                       std::complex<double> **evec_out,
                       bool require_evec)
{
    // Calculate phonon energy for the specific k-point given in fractional basis

    unsigned int i, j;
    std::complex<double> **dymat_k;

    memory->allocate(dymat_k, neval, neval);

    calc_analytic_k(xk_in, fc2_ext, dymat_k);

    if (nonanalytic) {

        // Add non-analytic correction

        std::complex<double> **dymat_na_k;

        memory->allocate(dymat_na_k, neval, neval);

        if (nonanalytic == 1) {
            calc_nonanalytic_k(xk_in, kvec_in, dymat_na_k);
        } else if (nonanalytic == 2) {
            calc_nonanalytic_k2(xk_in, kvec_in, dymat_na_k);
        }

        for (i = 0; i < neval; ++i) {
            for (j = 0; j < neval; ++j) {
                dymat_k[i][j] += dymat_na_k[i][j];
            }
        }
        memory->deallocate(dymat_na_k);
    }

    // Force the dynamical matrix be real when k point is
    // zone-center or zone-boundaries.

    if (std::sqrt(std::pow(std::fmod(xk_in[0], 0.5), 2.0)
        + std::pow(std::fmod(xk_in[1], 0.5), 2.0)
        + std::pow(std::fmod(xk_in[2], 0.5), 2.0)) < eps) {

        for (i = 0; i < 3 * system->natmin; ++i) {
            for (j = 0; j < 3 * system->natmin; ++j) {
                dymat_k[i][j] = std::complex<double>(dymat_k[i][j].real(), 0.0);
            }
        }
    }

    char JOBZ;
    int INFO;
    double *RWORK;
    std::complex<double> *WORK;

    int LWORK = (2 * neval - 1) * 10;
    memory->allocate(RWORK, 3 * neval - 2);
    memory->allocate(WORK, LWORK);

    std::complex<double> *amat;
    memory->allocate(amat, neval * neval);

    unsigned int k = 0;
    int n = dynamical->neval;

    for (j = 0; j < neval; ++j) {
        for (i = 0; i < neval; ++i) {
            amat[k++] = dymat_k[i][j];
        }
    }

    memory->deallocate(dymat_k);

    if (require_evec) {
        JOBZ = 'V';
    } else {
        JOBZ = 'N';
    }

    // Perform diagonalization
    zheev_(&JOBZ, &UPLO, &n, amat, &n, eval_out, WORK, &LWORK, RWORK, &INFO);

    if (eigenvectors && require_evec) {
        k = 0;
        // Here we transpose the matrix evec_out so that 
        // evec_out[i] becomes phonon eigenvector of i-th mode.
        for (j = 0; j < neval; ++j) {
            for (i = 0; i < neval; ++i) {
                evec_out[j][i] = amat[k++];
            }
        }
    }

    memory->deallocate(RWORK);
    memory->deallocate(WORK);
    memory->deallocate(amat);
}


void Dynamical::eval_k_ewald(double *xk_in,
                             double *kvec_in,
                             std::vector<FcsClassExtent> fc2_in,
                             double *eval_out,
                             std::complex<double> **evec_out,
                             const bool require_evec) const
{
    //
    // Calculate phonon energy for the specific k-point given in fractional basis
    // Contributions from dipole-dipole interactions should be extracted from 'fc2_in'.
    //
    unsigned int i, j;
    int icrd, jcrd;
    std::complex<double> **dymat_k, **mat_longrange;

    memory->allocate(dymat_k, neval, neval);
    memory->allocate(mat_longrange, neval, neval);

    calc_analytic_k(xk_in, fc2_in, dymat_k);

    // Calculate Coulombic contributions including long-range interactions 
    ewald->add_longrange_matrix(xk_in, kvec_in, mat_longrange);

    // Add calculated dynamical matrix of Coulomb parts
    for (i = 0; i < system->natmin; ++i) {
        for (icrd = 0; icrd < 3; ++icrd) {
            for (j = 0; j < system->natmin; ++j) {
                for (jcrd = 0; jcrd < 3; ++jcrd) {
                    dymat_k[3 * i + icrd][3 * j + jcrd] += mat_longrange[3 * i + icrd][3 * j + jcrd];
                }
            }
        }
    }

    // Check acoustic sum rule
    if (xk_in[0] == 0.0 && xk_in[1] == 0.0 && xk_in[2] == 0.0) {
        for (i = 0; i < system->natmin; ++i) {
            for (icrd = 0; icrd < 3; ++icrd) {
                for (jcrd = 0; jcrd < 3; ++jcrd) {
                    std::complex<double> check = std::complex<double>(0.0, 0.0);
                    int count = 0;
                    for (j = 0; j < system->natmin; ++j) {
                        double mass = system->mass[system->map_p2s[i][0]] * system->mass[system->map_p2s[j][0]];
                        check += std::sqrt(mass) * dymat_k[3 * i + icrd][3 * j + jcrd];
                        count += 1;
                    }

                    if (std::abs(check) > eps12) {
                        std::cout << "(" << 3 * i + icrd << "," << jcrd << "): " << check << std::endl;
                        error->warn("ewald->eval_k_ewald", "Acoustic sum rule is broken.");
                    }
                }
            }
        }
    }

    char JOBZ;
    int INFO;
    double *RWORK;
    std::complex<double> *WORK;

    int LWORK = (2 * neval - 1) * 10;
    memory->allocate(RWORK, 3 * neval - 2);
    memory->allocate(WORK, LWORK);

    std::complex<double> *amat;
    memory->allocate(amat, neval * neval);

    unsigned int k = 0;
    int n = dynamical->neval;
    for (j = 0; j < neval; ++j) {
        for (i = 0; i < neval; ++i) {
            amat[k++] = dymat_k[i][j];
        }
    }

    memory->deallocate(dymat_k);

    if (require_evec) {
        JOBZ = 'V';
    } else {
        JOBZ = 'N';
    }

    // Perform diagonalization
    zheev_(&JOBZ, &UPLO, &n, amat, &n, eval_out, WORK, &LWORK, RWORK, &INFO);

    if (eigenvectors && require_evec) {
        k = 0;
        // Here we transpose the matrix evec_out so that 
        // evec_out[i] becomes phonon eigenvector of i-th mode.
        for (j = 0; j < neval; ++j) {
            for (i = 0; i < neval; ++i) {
                evec_out[j][i] = amat[k++];
            }
        }
    }

    memory->deallocate(RWORK);
    memory->deallocate(WORK);
    memory->deallocate(amat);
}


void Dynamical::calc_analytic_k(double *xk_in,
                                const std::vector<FcsClassExtent> &fc2_in,
                                std::complex<double> **dymat_out) const
{
    int i;

    auto nmode = 3 * system->natmin;

    double vec[3];
    std::complex<double> im(0.0, 1.0);
    std::complex<double> **ctmp;

    memory->allocate(ctmp, nmode, nmode);
    for (i = 0; i < nmode; ++i) {
        for (int j = 0; j < nmode; ++j) {
            dymat_out[i][j] = std::complex<double>(0.0, 0.0);
        }
    }

    for (const auto &it : fc2_in) {

        unsigned int atm1_p = it.atm1;
        unsigned int atm2_s = it.atm2;
        unsigned int xyz1 = it.xyz1;
        unsigned int xyz2 = it.xyz2;
        unsigned int icell = it.cell_s;

        unsigned int atm1_s = system->map_p2s[atm1_p][0];
        unsigned int atm2_p = system->map_s2p[atm2_s].atom_num;

        for (i = 0; i < 3; ++i) {
            vec[i] = system->xr_s[atm2_s][i] + xshift_s[icell][i]
                - system->xr_s[system->map_p2s[atm2_p][0]][i];
        }

        rotvec(vec, vec, system->lavec_s);
        rotvec(vec, vec, system->rlavec_p);

        double phase = vec[0] * xk_in[0] + vec[1] * xk_in[1] + vec[2] * xk_in[2];

        dymat_out[3 * atm1_p + xyz1][3 * atm2_p + xyz2]
            += it.fcs_val * std::exp(im * phase) / std::sqrt(system->mass[atm1_s] * system->mass[atm2_s]);
    }
}


void Dynamical::calc_nonanalytic_k(double *xk_in,
                                   double *kvec_na_in,
                                   std::complex<double> **dymat_na_out)
{
    // Calculate the non-analytic part of dynamical matrices 
    // by Parlinski's method.

    unsigned int i, j;
    unsigned int iat, jat;
    unsigned int natmin = system->natmin;
    double kepsilon[3];
    double kz1[3], kz2[3];
    double born_tmp[3][3];
    double xk_tmp[3], xdiff[3];
    std::complex<double> im(0.0, 1.0);


    for (i = 0; i < neval; ++i) {
        for (j = 0; j < neval; ++j) {
            dymat_na_out[i][j] = std::complex<double>(0.0, 0.0);
        }
    }

    rotvec(kepsilon, kvec_na_in, dielec);
    double denom = kvec_na_in[0] * kepsilon[0]
        + kvec_na_in[1] * kepsilon[1]
        + kvec_na_in[2] * kepsilon[2];

    if (denom > eps) {

        for (iat = 0; iat < natmin; ++iat) {
            unsigned int atm_p1 = system->map_p2s[iat][0];

            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    born_tmp[i][j] = borncharge[iat][i][j];
                }
            }

            rotvec(kz1, kvec_na_in, born_tmp, 'T');

            for (jat = 0; jat < natmin; ++jat) {
                unsigned int atm_p2 = system->map_p2s[jat][0];


                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        born_tmp[i][j] = borncharge[jat][i][j];
                    }
                }

                rotvec(kz2, kvec_na_in, born_tmp, 'T');

                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {

                        dymat_na_out[3 * iat + i][3 * jat + j]
                            = kz1[i] * kz2[j] / (denom * std::sqrt(system->mass[atm_p1] * system->mass[atm_p2]));

                    }
                }
            }
        }
    }

    rotvec(xk_tmp, xk_in, system->rlavec_p, 'T');
    double norm2 = xk_tmp[0] * xk_tmp[0] + xk_tmp[1] * xk_tmp[1] + xk_tmp[2] * xk_tmp[2];

    double factor = 8.0 * pi / system->volume_p * std::exp(-norm2 / std::pow(na_sigma, 2));

    for (i = 0; i < neval; ++i) {
        for (j = 0; j < neval; ++j) {
            dymat_na_out[i][j] *= factor;
        }
    }

    // Multiply an additional phase factor for the non-analytic term.

    for (iat = 0; iat < natmin; ++iat) {
        for (jat = 0; jat < natmin; ++jat) {

            for (i = 0; i < 3; ++i) {
                xdiff[i] = system->xr_s[system->map_p2s[iat][0]][i]
                    - system->xr_s[system->map_p2s[jat][0]][i];
            }

            rotvec(xdiff, xdiff, system->lavec_s);
            rotvec(xdiff, xdiff, system->rlavec_p);

            double phase = xk_in[0] * xdiff[0] + xk_in[1] * xdiff[1] + xk_in[2] * xdiff[2];

            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    dymat_na_out[3 * iat + i][3 * jat + j] *= exp(im * phase);
                }
            }
        }
    }
}


void Dynamical::calc_nonanalytic_k2(double *xk_in,
                                    double *kvec_na_in,
                                    std::complex<double> **dymat_na_out)
{
    // Calculate the non-analytic part of dynamical matrices 
    // by the mixed-space approach.

    unsigned int i, j;
    unsigned int natmin = system->natmin;
    double kepsilon[3];
    double kz1[3], kz2[3];
    double born_tmp[3][3];
    double vec[3];
    std::complex<double> im(0.0, 1.0);


    for (i = 0; i < neval; ++i) {
        for (j = 0; j < neval; ++j) {
            dymat_na_out[i][j] = std::complex<double>(0.0, 0.0);
        }
    }

    rotvec(kepsilon, kvec_na_in, dielec);
    double denom = kvec_na_in[0] * kepsilon[0]
        + kvec_na_in[1] * kepsilon[1]
        + kvec_na_in[2] * kepsilon[2];

    if (denom > eps) {

        for (unsigned int iat = 0; iat < natmin; ++iat) {
            unsigned int atm_p1 = system->map_p2s[iat][0];

            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    born_tmp[i][j] = borncharge[iat][i][j];
                }
            }

            rotvec(kz1, kvec_na_in, born_tmp, 'T');

            for (unsigned int jat = 0; jat < natmin; ++jat) {
                unsigned int atm_p2 = system->map_p2s[jat][0];


                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        born_tmp[i][j] = borncharge[jat][i][j];
                    }
                }

                rotvec(kz2, kvec_na_in, born_tmp, 'T');

                std::complex<double> exp_phase = std::complex<double>(0.0, 0.0);

                for (i = 0; i < system->ntran; ++i) {

                    std::complex<double> exp_phase_tmp = std::complex<double>(0.0, 0.0);
                    unsigned int atm_s2 = system->map_p2s[jat][i];

                    // Average over mirror atoms

                    for (j = 0; j < mindist_list[iat][atm_s2].size(); ++j) {
                        unsigned int cell = mindist_list[iat][atm_s2][j];

                        for (unsigned int k = 0; k < 3; ++k) {
                            vec[k] = system->xr_s[system->map_p2s[jat][i]][k] + xshift_s[cell][k]
                                - system->xr_s[atm_p2][k];
                        }

                        rotvec(vec, vec, system->lavec_s);
                        rotvec(vec, vec, system->rlavec_p);

                        double phase = vec[0] * xk_in[0] + vec[1] * xk_in[1] + vec[2] * xk_in[2];

                        exp_phase_tmp += std::exp(im * phase);
                    }
                    exp_phase += exp_phase_tmp / static_cast<double>(mindist_list[iat][atm_s2].size());
                }
                exp_phase /= static_cast<double>(system->ntran);

                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        dymat_na_out[3 * iat + i][3 * jat + j]
                            = kz1[i] * kz2[j] / (denom * std::sqrt(system->mass[atm_p1] * system->mass[atm_p2]))
                            * exp_phase;
                    }
                }
            }
        }
    }

    double factor = 8.0 * pi / system->volume_p;

    for (i = 0; i < neval; ++i) {
        for (j = 0; j < neval; ++j) {
            dymat_na_out[i][j] *= factor;
        }
    }
}


void Dynamical::diagonalize_dynamical_all()
{
    unsigned int nk = kpoint->nk;
    bool require_evec;

    if (mympi->my_rank == 0) {
        std::cout << std::endl << " Diagonalizing dynamical matrices for all k points ... ";
    }

    memory->allocate(eval_phonon, nk, neval);
    if (eigenvectors) {
        require_evec = true;
        memory->allocate(evec_phonon, nk, neval, neval);
    } else {
        require_evec = false;
        memory->allocate(evec_phonon, nk, 1, 1);
    }

    // Calculate phonon eigenvalues and eigenvectors for all k-points
#ifdef _OPENMP
#pragma omp parallel for
#endif
    for (int ik = 0; ik < nk; ++ik) {
        if (nonanalytic == 3) {
            eval_k_ewald(kpoint->xk[ik],
                         kpoint->kvec_na[ik],
                         ewald->fc2_without_dipole,
                         eval_phonon[ik],
                         evec_phonon[ik],
                         require_evec);
        } else {
            eval_k(kpoint->xk[ik],
                   kpoint->kvec_na[ik],
                   fcs_phonon->fc2_ext,
                   eval_phonon[ik],
                   evec_phonon[ik],
                   require_evec);
        }
        // Phonon energy is the square-root of the eigenvalue 
        for (unsigned int is = 0; is < neval; ++is) {
            eval_phonon[ik][is] = freq(eval_phonon[ik][is]);
        }
    }

    if (band_connection > 0 && kpoint->kpoint_mode == 1) {
        memory->allocate(index_bconnect, nk, neval);
        connect_band_by_eigen_similarity(evec_phonon, index_bconnect);
    }

    if (mympi->my_rank == 0) {
        std::cout << "done!" << std::endl;
    }

    if (kpoint->kpoint_mode == 2 && phon->mode == "RTA") {
        detect_imaginary_branches(dynamical->eval_phonon);
    }
}


void Dynamical::modify_eigenvectors() const
{
    bool *flag_done;
    unsigned int ik;
    unsigned int js;
    std::complex<double> *evec_tmp;

    auto nk = kpoint->nk;
    auto ns = neval;

    /*   if (mympi->my_rank == 0) {
           std::cout << " **********      NOTICE      ********** " << std::endl;
           std::cout << " For the brevity of the calculation, " << std::endl;
           std::cout << " phonon eigenvectors will be modified" << std::endl;
           std::cout << " so that e_{-ks}^{mu} = (e_{ks}^{mu})^{*}. " << std::endl;
       }*/

    memory->allocate(flag_done, nk);
    memory->allocate(evec_tmp, ns);

    for (ik = 0; ik < nk; ++ik) flag_done[ik] = false;

    for (ik = 0; ik < nk; ++ik) {

        if (!flag_done[ik]) {

            unsigned int nk_inv = kpoint->knum_minus[ik];

            for (unsigned int is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    evec_tmp[js] = evec_phonon[ik][is][js];
                }

                for (js = 0; js < ns; ++js) {
                    evec_phonon[nk_inv][is][js] = std::conj(evec_tmp[js]);
                }
            }

            flag_done[ik] = true;
            flag_done[nk_inv] = true;
        }
    }

    memory->deallocate(flag_done);
    memory->deallocate(evec_tmp);

    MPI_Barrier(MPI_COMM_WORLD);
    //if (mympi->my_rank == 0) {
    //    std::cout << " done !" << std::endl;
    //    std::cout << " **************************************" << std::endl;
    //}
}

void Dynamical::setup_dielectric(const unsigned int verbosity) 
{
    if (borncharge) memory->deallocate(borncharge);

    memory->allocate(borncharge, system->natmin, 3, 3);
    if (mympi->my_rank == 0) load_born(symmetrize_borncharge);

    MPI_Bcast(&dielec[0][0], 9, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    MPI_Bcast(&borncharge[0][0][0], 9 * system->natmin, MPI_DOUBLE, 0, MPI_COMM_WORLD);
}


void Dynamical::load_born(const unsigned int flag_symmborn, 
                          const unsigned int verbosity)
{
    // Read the dielectric tensor and born effective charges from file_born

    unsigned int i, j, k;
    double sum_born[3][3];
    std::ifstream ifs_born;

    ifs_born.open(file_born.c_str(), std::ios::in);
    if (!ifs_born) error->exit("load_born", "cannot open file_born");

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            ifs_born >> dielec[i][j];
        }
    }

    for (i = 0; i < system->natmin; ++i) {
        for (j = 0; j < 3; ++j) {
            for (k = 0; k < 3; ++k) {
                ifs_born >> borncharge[i][j][k];
            }
        }
    }
    ifs_born.close();

    if (verbosity > 0) {
        std::cout << "  Dielectric constants and Born effective charges are read from "
            << file_born << "." << std::endl << std::endl;
        std::cout << "  Dielectric constant tensor in Cartesian coordinate : " << std::endl;
        for (i = 0; i < 3; ++i) {
            for (j = 0; j < 3; ++j) {
                std::cout << std::setw(15) << dielec[i][j];
            }
            std::cout << std::endl;
        }
        std::cout << std::endl;

        std::cout << "  Born effective charge tensor in Cartesian coordinate" << std::endl;
        for (i = 0; i < system->natmin; ++i) {
            std::cout << "  Atom" << std::setw(5) << i + 1 << "("
                << std::setw(3) << system->symbol_kd[system->kd[system->map_p2s[i][0]]] << ") :" << std::endl;

            for (j = 0; j < 3; ++j) {
                for (k = 0; k < 3; ++k) {
                    std::cout << std::setw(15) << std::fixed
                        << std::setprecision(6) << borncharge[i][j][k];
                }
                std::cout << std::endl;
            }
        }
    }


    // Check if the ASR is satisfied. If not, enforce it.

    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            sum_born[i][j] = 0.0;
            for (k = 0; k < system->natmin; ++k) {
                sum_born[i][j] += borncharge[k][i][j];
            }
        }
    }

    double res = 0.0;
    for (i = 0; i < 3; ++i) {
        for (j = 0; j < 3; ++j) {
            res += std::pow(sum_born[i][j], 2);
        }
    }

    if (res > eps10) {
        if (verbosity > 0) {
            std::cout << std::endl;
            std::cout << "  WARNING: Born effective charges do not satisfy the acoustic sum rule." << std::endl;
            std::cout << "           The born effective charges are modified to satisfy the ASR." << std::endl;
        }
        
        for (i = 0; i < system->natmin; ++i) {
            for (j = 0; j < 3; ++j) {
                for (k = 0; k < 3; ++k) {
                    borncharge[i][j][k] -= sum_born[j][k] / static_cast<double>(system->natmin);
                }
            }
        }
    }

    if (flag_symmborn) {

        // Symmetrize Born effective charges. Necessary to avoid the violation of ASR 
        // particularly for NONANALYTIC=3 (Ewald summation).

        int iat;
        double ***born_sym;
        double rot[3][3];

        memory->allocate(born_sym, system->natmin, 3, 3);

        for (iat = 0; iat < system->natmin; ++iat) {
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    born_sym[iat][i][j] = 0.0;
                }
            }
        }

        for (int isym = 0; isym < symmetry->SymmListWithMap.size(); ++isym) {
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    rot[i][j] = symmetry->SymmListWithMap[isym].rot[3 * i + j];
                }
            }

            for (iat = 0; iat < system->natmin; ++iat) {
                int iat_sym = symmetry->SymmListWithMap[isym].mapping[iat];

                for (i = 0; i < 3; ++i) {
                    for (j = 0; j < 3; ++j) {
                        for (k = 0; k < 3; ++k) {
                            for (int m = 0; m < 3; ++m) {
                                born_sym[iat_sym][i][j] += rot[i][k] * rot[j][m] * borncharge[iat][k][m];
                            }
                        }
                    }
                }
            }
        }

        for (iat = 0; iat < system->natmin; ++iat) {
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    born_sym[iat][i][j] /= static_cast<double>(symmetry->SymmListWithMap.size());
                }
            }
        }

        // Check if the Born effective charges given by the users satisfy the symmetry.

        double diff_sym = 0.0;
        for (iat = 0; iat < system->natmin; ++iat) {
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    diff_sym = std::max<double>(diff_sym, std::abs(borncharge[iat][i][j] - born_sym[iat][i][j]));
                }
            }
        }

        if (diff_sym > 0.5 && verbosity > 0) {
            std::cout << std::endl;
            std::cout << "  WARNING: Born effective charges are inconsistent with the crystal symmetry." << std::endl;
        }

        for (iat = 0; iat < system->natmin; ++iat) {
            for (i = 0; i < 3; ++i) {
                for (j = 0; j < 3; ++j) {
                    borncharge[iat][i][j] = born_sym[iat][i][j];
                }
            }
        }
        memory->deallocate(born_sym);

        if (verbosity > 0) {
            if (diff_sym > eps8 || res > eps10) {
                std::cout << std::endl;
                std::cout << "  Symmetrized Born effective charge tensor in Cartesian coordinate." << std::endl;
                for (i = 0; i < system->natmin; ++i) {
                    std::cout << "  Atom" << std::setw(5) << i + 1 << "("
                        << std::setw(3) << system->symbol_kd[system->kd[system->map_p2s[i][0]]] << ") :" << std::endl;

                    for (j = 0; j < 3; ++j) {
                        for (k = 0; k < 3; ++k) {
                            std::cout << std::setw(15) << borncharge[i][j][k];
                        }
                        std::cout << std::endl;
                    }
                }
            }
        }
    }
    std::cout << std::scientific;
}


double Dynamical::fold(const double x) const
{
    return x - static_cast<double>(nint(x));
}


double Dynamical::freq(const double x) const
{
    // Special treatment to avoid the divergence of computation.
    if (std::abs(x) < eps) return eps15;

    if (x > 0.0) return std::sqrt(x);

    return -std::sqrt(-x);
}


void Dynamical::calc_participation_ratio_all(std::complex<double> ***evec,
                                             double **ret,
                                             double ***ret_all) const
{
    auto nk = kpoint->nk;
    auto ns = dynamical->neval;
    auto natmin = system->natmin;

    double *atomic_pr;

    memory->allocate(atomic_pr, natmin);

    for (auto ik = 0; ik < nk; ++ik) {
        for (auto is = 0; is < ns; ++is) {
            calc_atomic_participation_ratio(evec[ik][is], atomic_pr);

            double sum = 0.0;

            for (auto iat = 0; iat < natmin; ++iat) {
                sum += atomic_pr[iat];
                ret_all[ik][is][iat] = atomic_pr[iat];
            }

            ret[ik][is] = sum * sum;
        }
    }

    memory->deallocate(atomic_pr);
}


void Dynamical::calc_atomic_participation_ratio(std::complex<double> *evec,
                                                double *ret) const
{
    unsigned int iat;
    auto natmin = system->natmin;

    for (iat = 0; iat < natmin; ++iat) ret[iat] = 0.0;

    for (iat = 0; iat < natmin; ++iat) {
        ret[iat] = (std::norm(evec[3 * iat])
            + std::norm(evec[3 * iat + 1])
            + std::norm(evec[3 * iat + 2])) / system->mass[system->map_p2s[iat][0]];
    }

    double sum = 0.0;

    for (iat = 0; iat < natmin; ++iat) sum += ret[iat] * ret[iat];

    for (iat = 0; iat < natmin; ++iat)
        ret[iat] /= std::sqrt(static_cast<double>(natmin) * sum);
}


void Dynamical::connect_band_by_eigen_similarity(std::complex<double> ***evec,
                                                 int **index_sorted) const
{
    int ik, is, js;
    auto nk = kpoint->nk;
    auto ns = neval;
    std::vector<int> index;
    std::complex<double> **evec_tmp;
    std::vector<std::vector<double>> abs_similarity;
    std::complex<double> dprod;
    std::vector<int> found;

    memory->allocate(evec_tmp, ns, ns);

    for (ik = 0; ik < nk; ++ik) {
        for (is = 0; is < ns; ++is) {
            index_sorted[ik][is] = 0;
        }
    }

    index.resize(ns);
    found.resize(ns);
    abs_similarity.resize(ns);
    for (is = 0; is < ns; ++is) {
        abs_similarity[is].resize(ns);
    }

    for (int i = 0; i < ns; ++i) index[i] = i;

    for (ik = 0; ik < nk; ++ik) {

        if (ik == 0) {
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    if (is == js) {
                        abs_similarity[is][js] = 1.0;
                    } else {
                        abs_similarity[is][js] = 0.0;
                    }
                }
            }
        } else {
#ifdef _OPENMP
#pragma omp parallel for private(js, dprod)
#endif
            for (is = 0; is < ns; ++is) {
                for (js = 0; js < ns; ++js) {
                    dprod = std::complex<double>(0.0, 0.0);
                    for (int ks = 0; ks < ns; ++ks) {
                        dprod += std::conj(evec[ik][is][ks]) * evec_tmp[js][ks];
                    }
                    abs_similarity[is][js] = std::abs(dprod);
                }
            }
        }

        for (auto &v : found) v = 0;

        for (is = 0; is < ns; ++is) {

            // Argsort abs_similarity[is] (use C++11 lambda)
            iota(index.begin(), index.end(), 0);
            std::sort(index.begin(), index.end(),
                      [&abs_similarity, is](int i1,
                                            int i2)
                      {
                          return abs_similarity[is][i1] > abs_similarity[is][i2];
                      });

            int loc = index[0];
            index_sorted[ik][loc] = is;
            found[loc] = 1;
            for (js = 0; js < ns; ++js) abs_similarity[js][loc] = -1.0;
            for (js = 0; js < ns; ++js) {
                evec_tmp[loc][js] = evec[ik][is][js];
            }
        }

        if (std::any_of(found.begin(), found.end(), [](int i1) { return i1 == 0; })) {
            error->exit("connect_band_by_eigen_similarity",
                        "Could not identify the connection.");
        }

    }
    memory->deallocate(evec_tmp);
}


void Dynamical::detect_imaginary_branches(double **eval)
{
    int ik, is;
    auto nk = kpoint->nk;
    auto ns = dynamical->neval;
    auto nks = ns * nk;
    int knum;
    double omega;

    bool is_anyof_imaginary = false;
    if (mympi->my_rank == 0) {

        memory->allocate(is_imaginary, kpoint->nk_irred, ns);

        for (ik = 0; ik < kpoint->nk_irred; ++ik) {
            for (is = 0; is < ns; ++is) {
                knum = kpoint->kpoint_irred_all[ik][0].knum;
                omega = eval[knum][is];

                if (omega < 0.0) {
                    is_imaginary[ik][is] = true;
                    is_anyof_imaginary = true;
                } else {
                    is_imaginary[ik][is] = false;
                }
            }
        }

        if (is_anyof_imaginary) {
            int count = 0;
            std::cout << std::endl;
            std::cout << " WARNING: Imaginary frequency detected at the following branches:" << std::endl;
            for (ik = 0; ik < kpoint->nk_irred; ++ik) {
                for (is = 0; is < ns; ++is) {
                    if (is_imaginary[ik][is]) {
                        int ndup = kpoint->kpoint_irred_all[ik].size();
                        count += ndup;
                        for (int i = 0; i < ndup; ++i) {
                            knum = kpoint->kpoint_irred_all[ik][i].knum;
                            omega = eval[knum][is];
                            for (int j = 0; j < 3; ++j) {
                                std::cout << std::setw(15) << kpoint->xk[knum][j];
                            }
                            std::cout << std::setw(4) << is + 1 << " :"
                                << std::setw(10) << std::fixed
                                << writes->in_kayser(omega) << " (cm^-1)" << std::endl;
                            std::cout << std::scientific;
                        }
                    }
                }
            }
            std::cout << std::setw(5) << count << " imaginary branches out of "
                << std::setw(5) << nks << " total branches." << std::endl;
            std::cout << std::endl;
            std::cout << " Phonon-phonon scattering rate and thermal conductivity involving these" << std::endl;
            std::cout << " imaginary branches will be treated as zero in the following calculations." << std::endl;
            std::cout << " If imaginary branches are acoustic phonons at Gamma point (0, 0, 0), " << std::endl;
            std::cout << " you can safely ignore this message." << std::endl << std::endl << std::flush;
        }
    }
}

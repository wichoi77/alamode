/*
 phonon_velocity.h

 Copyright (c) 2014 Terumasa Tadano

 This file is distributed under the terms of the MIT license.
 Please see the file 'LICENCE.txt' in the root directory 
 or http://opensource.org/licenses/mit-license.php for information.
*/

#pragma once

#include "pointers.h"

namespace PHON_NS {
    class Phonon_velocity: protected Pointers {
    public:
        Phonon_velocity(class PHON *);
        ~Phonon_velocity();

        void calc_group_velocity(const int);
        void calc_phonon_vel_band();
        void phonon_vel_k(double *, double **);

        bool print_velocity;
        double **phvel;

    private:
        double diff(double *, const unsigned int, double);

        void calc_phonon_vel_band(double **);
        void calc_phonon_vel_mesh(double **);
    };
}

// MIT License
//
// MEII - MAHI Exo-II Extension of MEL, the MAHI Exoskeleton Library
// Copyright (c) 2018 Mechatronics and Haptic Interfaces Lab - Rice University
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// Author(s): Craig McDonald (craig.g.mcdonald@gmail.com)

#ifndef MEII_RECTIFIER_HPP
#define MEII_RECTIFIER_HPP

#include <MEL/Math/Process.hpp>
#include <vector>

namespace meii {

    //==============================================================================
    // CLASS DECLARATION
    //==============================================================================

    class Rectifier : public mel::Process {

    public:

        /// Constructor
        Rectifier();

        /// applies the process operation for one sample
        double update(const double x, const mel::Time& current_time = mel::Time::Zero) override;

        /// resets internal memory
        void reset() override;

    private:

    };

} // namespace meii

#endif // MEII_RECTIFIER_HPP
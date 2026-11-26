#pragma once

//! Radio-button indices for the mutually exclusive NDT/ICP engine flags.
struct Step2SolverSelection
{
    int optimizer;
    int pose_convention;
    int rotation;

    template<typename Solver>
    static Step2SolverSelection read(const Solver& solver)
    {
        return { solver.is_gauss_newton ? 0 : 1, solver.is_cw ? 0 : 1, solver.is_tait_bryan_angles ? 0 : (solver.is_quaternion ? 1 : 2) };
    }

    template<typename Solver>
    void write(Solver& solver) const
    {
        solver.is_gauss_newton = optimizer == 0;
        solver.is_levenberg_marguardt = optimizer == 1;
        solver.is_cw = pose_convention == 0;
        solver.is_wc = pose_convention == 1;
        solver.is_tait_bryan_angles = rotation == 0;
        solver.is_quaternion = rotation == 1;
        solver.is_rodrigues = rotation == 2;
    }
};

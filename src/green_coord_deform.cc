#include "green_coord_deform.h"

#include <zjucad/matrix/matrix.h>
#include <jtflib/mesh/io.h>
#include <zjucad/matrix/io.h>

#include "vtk.h"

using namespace std;
using namespace zjucad::matrix;
using namespace Eigen;

namespace geom_deform {

green_deform_2d::green_deform_2d() {}

int green_deform_2d::load_sample_points(const char *file) {
    matrix<size_t> cell;
    matrix<double> nods;
    if ( jtf::mesh::load_obj(file, cell, nods) ) {
        return __LINE__;
    }
    cell_.resize(cell.size(1), cell.size(2));
    nods_.resize(2, nods.size(2));
    std::copy(cell.begin(), cell.end(), cell_.data());
#pragma omp parallel for
    for (size_t i = 0; i < nods.size(2); ++i) {
        nods_(0, i) = nods(0, i);
        nods_(1, i) = nods(2, i);
    }
    return 0;
}

int green_deform_2d::load_cage(const char *file) {
    // parse manually
    ifstream is(file);
    if ( is.fail() ) {
        cerr << "# can not open " << file << "\n";
        return __LINE__;
    }

    size_t ele_num;
    string CELL;
    is >> CELL >> ele_num;
    cage_cell_.resize(2, ele_num);
    for (size_t i = 0; i < ele_num; ++i)
        is >> cage_cell_(0, i) >> cage_cell_(1, i);

    size_t nods_num;
    string NODES;
    is >> NODES >> nods_num;
    cage_nods_.resize(2, nods_num);
    for (size_t i = 0; i < nods_num; ++i)
        is >> cage_nods_(0, i) >> cage_nods_(1, i);

//    // get rest segment element length
//    len_.resize(ele_num);

    // calc initial normal
    cage_normal_.resize(2, cage_cell_.cols());
    calc_outward_normal();
    return 0;
}

int green_deform_2d::calc_outward_normal() {
#pragma omp parallel for
    for (size_t i = 0; i < cage_cell_.cols(); ++i) {
        Vector2d dir = cage_nods_.col(cage_cell_(1, i)) - cage_nods_.col(cage_cell_(0, i));
        dir /= dir.norm();
        cage_normal_(0, i) = -dir[1];
        cage_normal_(1, i) = dir[0];
    }
    return 0;
}

int green_deform_2d::calc_green_coords() {
    return 0;
}

int green_deform_2d::move_cage(const size_t id, const double *dx) {
    Vector2d disp(dx[0], dx[1]);
    cage_nods_.col(id) += disp;
    return 0;
}

int green_deform_2d::update_cage_edge_length() {
    return 0;
}

int green_deform_2d::deform() {
    calc_outward_normal();
    update_cage_edge_length();
    VectorXd ratio = curr_len_.cwiseQuotient(rest_len_);
    return 0;
}

int green_deform_2d::dump(const char *file) {
    MatrixXd nods_3d(3, nods_.cols());
#pragma omp parallel for
    for (size_t i = 0; i < nods_.cols(); ++i) {
        nods_3d(0, i) = nods_(0, i);
        nods_3d(1, i) = 0.0;
        nods_3d(2, i) = nods_(1, i);
    }
    ofstream os(file);
    tri2vtk(os, nods_3d.data(), nods_3d.cols(), cell_.data(), cell_.cols());
    return 0;
}


int green_deform_2d::dump_cage(const char *file) {
    MatrixXd cage_nods_3d(3, cage_nods_.cols());
#pragma omp parallel for
    for (size_t i = 0; i < cage_nods_.cols(); ++i) {
        cage_nods_3d(0, i) = cage_nods_(0, i);
        cage_nods_3d(1, i) = 0.0;
        cage_nods_3d(2, i) = cage_nods_(1, i);
    }
    ofstream os(file);
    line2vtk(os, cage_nods_3d.data(), cage_nods_3d.cols(), cage_cell_.data(), cage_cell_.cols());
    return 0;
}

int green_deform_2d::dump_normal(const char *file) {
    matrix<size_t> normal_cell(2, cage_cell_.cols());
    for (size_t i = 0; i < normal_cell.size(); ++i)
        normal_cell[i] = i;
    MatrixXd normal_nods(3, 2 * normal_cell.size(2));
    normal_nods.setZero();
#pragma omp parallel for
    for (size_t i = 0; i < normal_nods.cols()/2; ++i) {
        Vector2d mid = 0.5 * (cage_nods_.col(cage_cell_(0, i)) + cage_nods_.col(cage_cell_(1, i)));
        Vector2d end = mid + cage_normal_.col(i);
        normal_nods(0, 2*i+0) = mid[0];
        normal_nods(2, 2*i+0) = mid[1];
        normal_nods(0, 2*i+1) = end[0];
        normal_nods(2, 2*i+1) = end[1];
    }
    ofstream os(file);
    line2vtk(os, normal_nods.data(), normal_nods.cols(), normal_cell.begin(), normal_cell.size(2));
    return 0;
}

}

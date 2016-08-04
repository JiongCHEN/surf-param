#ifndef L1_POLYCUBE_H
#define L1_POLYCUBE_H

#include <boost/property_tree/ptree.hpp>
#include <zjucad/matrix/matrix.h>

namespace riemann {

using mati_t=zjucad::matrix::matrix<size_t>;
using matd_t=zjucad::matrix::matrix<double>;
using boost::property_tree::ptree;

template <typename T>
class Functional;

class surf_normal_align_energy;
class tet_distortion_energy;

class polycube_solver
{
public:
  polycube_solver(const mati_t &tets, const matd_t &nods, ptree &pt);
  int deform(matd_t &x) const;
  int unit_test() const;
private:
  double eval_polycube_error(const matd_t &x) const;
private:
  const mati_t &tets_;
  mati_t surf_;
  ptree &pt_;
  std::vector<std::shared_ptr<Functional<double>>> buffer_;
  std::shared_ptr<Functional<double>> energy_, area_cons_;
  std::shared_ptr<surf_normal_align_energy> alig_;
  std::shared_ptr<tet_distortion_energy> arap_;
};

}

#endif

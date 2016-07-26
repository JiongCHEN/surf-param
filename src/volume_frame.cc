#include "volume_frame.h"

#include <zjucad/matrix/itr_matrix.h>
#include <jtflib/mesh/mesh.h>
#include <jtflib/mesh/util.h>
#include <hjlib/math/blas_lapack.h>
#include <zjucad/matrix/lapack.h>
#include <Eigen/Sparse>
#include <Eigen/CholmodSupport>

#include "def.h"
#include "config.h"
#include "grad_operator.h"
#include "lbfgs_solve.h"
#include "sh_zyz_convert.h"
#include "util.h"
#include "petsc_linear_solver.h"
#include "geometry_extend.h"

using namespace std;
using namespace zjucad::matrix;
using namespace jtf::mesh;
using namespace Eigen;

namespace riemann {

extern "C" {

  void cubic_sym_smooth_(double *val, const double *abc, const double *CR, const double *vol);
  void cubic_sym_smooth_jac_(double *jac, const double *abc, const double *CR, const double *vol);
    
  void cubic_sym_align_(double *val, const double *abc, const double *rnz, const double *area);
  void cubic_sym_align_jac_(double *jac, const double *abc, const double *rnz, const double *area);

  void cubic_smooth_sh_coef_(double *val, const double *F, const double *CR, const double *vol);
  void cubic_smooth_sh_coef_jac_(double *jac, const double *F, const double *CR, const double *vol);
  void cubic_smooth_sh_coef_hes_(double *hes, const double *F, const double *CR, const double *vol);

  void cubic_align_sh_coef_(double *val, const double *F, const double *Rnz, const double *area);
  void cubic_align_sh_coef_jac_(double *jac, const double *F, const double *Rnz, const double *area);
  void cubic_align_sh_coef_hes_(double *hes, const double *F, const double *Rnz, const double *area);

  void cubic_sym_smooth_tet_(double *val, const double *abc, const double *stiff);
  void cubic_sym_smooth_tet_jac_(double *jac, const double *abc, const double *stiff);

  void poly_smooth_tet_(double *val, const double *abc, const double *stiff);
  void poly_smooth_tet_jac_(double *jac, const double *abc, const double *stiff);

  void l1_cubic_sym_smooth_(double *val, const double *Rab, const double *eps, const double *stiff);
  void l1_cubic_sym_smooth_jac_(double *jac, const double *Rab, const double *eps, const double *stiff);

  void frm_orth_term_(double *val, const double *R, const double *stiff);
  void frm_orth_term_jac_(double *jac, const double *R, const double *stiff);

}

static inline void normal2zyz(const double *n, double *zyz) {
  zyz[0] = -atan2(n[1], n[0]);
  zyz[1] = -acos(n[2]);
  zyz[2] = 0;
}

void convert_zyz_to_mat(const VectorXd &abc, VectorXd &mat) {
  const size_t elem_num = abc.size()/3;
  if ( mat.size() != 9*elem_num )
    mat.resize(9*elem_num);
  #pragma omp parallel for
  for (size_t i = 0; i < elem_num; ++i) 
    Map<Matrix3d>(&mat[9*i], 3, 3) = RZ(abc[3*i+2])*RY(abc[3*i+1])*RZ(abc[3*i+0]);
}

//===============================================================================

class SH_smooth_energy_tet : public Functional<double>
{
public:
  SH_smooth_energy_tet(const mati_t &tets, const matd_t &nods, const double w)
      : tets_(tets), w_(w), dim_(3*tets.size(2)) {
    matd_t volume = zeros<double>(tets.size(2), 1); {
      #pragma omp parallel for
      for (size_t i = 0; i < tets.size(2); ++i) {
        matd_t Ds = nods(colon(), tets(colon(1, 3), i))-nods(colon(), tets(0, i))*ones<double>(1, 3);
        volume[i] = fabs(det(Ds))/6.0;
      }
    }
    
    shared_ptr<face2tet_adjacent> f2t(face2tet_adjacent::create(tets));
    vector<size_t> buffer;
    for (size_t i = 0; i < f2t->face2tet_.size(); ++i) {
      const pair<size_t, size_t> ts = f2t->face2tet_[i];
      if ( !f2t->is_outside_face(ts) ) {
        buffer.push_back(ts.first);
        buffer.push_back(ts.second);
      }
    }
    adjt_.resize(2, buffer.size()/2);
    std::copy(buffer.begin(), buffer.end(), adjt_.begin());

    stiff_.resize(adjt_.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < stiff_.size(); ++i) {
        const double dist = norm(
            nods(colon(), tets(colon(), adjt_(0, i)))*ones<double>(4, 1)/4.0-
            nods(colon(), tets(colon(), adjt_(1, i)))*ones<double>(4, 1)/4.0);
        stiff_[i] = (volume[adjt_(0, i)]+volume[adjt_(1, i)])/(dist*dist);
      }
      double total = sum(stiff_);
      stiff_ /= total;
    }
  }
  virtual ~SH_smooth_energy_tet() {}
  virtual size_t Nx() const {
    return dim_;
  }
  virtual int Val(const double *abc, double *val) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    matd_t abcs = zeros<double>(3, 2); double value = 0;
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      abcs = ABC(colon(), adjt_(colon(), i));
      cubic_sym_smooth_tet_(&value, &abcs[0], &stiff_[i]);
      *val += w_*value;
    }
    return 0;
  }
  virtual int Gra(const double *abc, double *gra) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    itr_matrix<double *> G(3, dim_/3, gra);
    matd_t abcs = zeros<double>(3, 2), g = zeros<double>(3, 2);
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      abcs = ABC(colon(), adjt_(colon(), i));
      cubic_sym_smooth_tet_jac_(&g[0], &abcs[0], &stiff_[i]);
      G(colon(), adjt_(colon(), i)) += w_*g;
    }
    return 0;
  }
  virtual int Hes(const double *abc, vector<Triplet<double>> *hes) const {
    return __LINE__;
  }
  int ValSH(const double *f, double *val) const {
    itr_matrix<const double *> Fs(9, dim_/3, f);
    matd_t fs = zeros<double>(9, 2), dif = zeros<double>(9, 1);
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      fs = Fs(colon(), adjt_(colon(), i));
      dif = fs(colon(), 0)-fs(colon(), 1);
      *val += w_*stiff_[i]*dot(dif, dif);
    }
    return 0;
  }
  int GraSH(const double *f, double *gra) const {
    itr_matrix<const double *> Fs(9, dim_/3, f);
    itr_matrix<double *> G(9, dim_/3, gra);
    matd_t fs = zeros<double>(9, 2);
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      fs = Fs(colon(), adjt_(colon(), i));
      G(colon(), adjt_(0, i)) += 2.0*w_*stiff_[i]*(fs(colon(), 0)-fs(colon(), 1));
      G(colon(), adjt_(1, i)) += 2.0*w_*stiff_[i]*(fs(colon(), 1)-fs(colon(), 0));
    }
    return 0;
  }
  int HesSH(const double *f, vector<Triplet<double>> *hes) const {
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      const size_t l = adjt_(0, i), r = adjt_(1, i);
      const double entry = 2.0*w_*stiff_[i];
      add_diag_block<double, 9>(l, l, entry, hes);
      add_diag_block<double, 9>(l, r, -entry, hes);
      add_diag_block<double, 9>(r, l, -entry, hes);
      add_diag_block<double, 9>(r, r, entry, hes);
    }
    return 0;
  }
protected:
  const mati_t &tets_;
  const double w_;
  const size_t dim_;

  mati_t adjt_;
  matd_t stiff_;
};

class SH_align_energy_tet : public Functional<double>
{
public:
  SH_align_energy_tet(const mati_t &tets, const matd_t &nods, const double w)
      : tets_(tets), w_(w), dim_(3*tets.size(2)) {
    shared_ptr<face2tet_adjacent> f2t(face2tet_adjacent::create(tets));
    mati_t surf; bool check_order = true;
    jtf::mesh::get_outside_face(*f2t, surf, check_order, &nods);
  
    stiff_.resize(surf.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < surf.size(2); ++i)
        stiff_[i] = jtf::mesh::cal_face_area(nods(colon(), surf(colon(), i)));
      double sum_area = sum(stiff_);
      stiff_ /= sum_area;
    }
    
    adjt_.resize(surf.size(2));
    zyz_.resize(3, surf.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < surf.size(2); ++i) {
        matd_t n = zeros<double>(3, 1);
        jtf::mesh::cal_face_normal(nods(colon(), surf(colon(), i)), n);
        normal2zyz(&n[0], &zyz_(0, i));

        const pair<size_t, size_t> t = f2t->query(surf(0, i), surf(1, i), surf(2, i));
        adjt_[i] = (t.first == -1) ? t.second : t.first;
      }
    }
  }
  size_t Nx() const {
    return dim_;
  }
  int Val(const double *abc, double *val) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    double value = 0;
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      cubic_sym_align_(&value, &ABC(0, tid), &zyz_(0, i), &stiff_[i]);
      *val += w_*value;
    }
    return 0;
  }
  int Gra(const double *abc, double *gra) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    itr_matrix<double *> G(3, dim_/3, gra);
    matd_t g = zeros<double>(3, 1);
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      cubic_sym_align_jac_(&g[0], &ABC(0, tid), &zyz_(0, i), &stiff_[i]);
      G(colon(), tid) += w_*g;
    }
    return 0;
  }
  int Hes(const double *abc, vector<Triplet<double>> *hes) const {
    return __LINE__;
  }
  int ValSH(const double *f, double *val) const {
    itr_matrix<const double *> Fs(9, dim_/3, f);
    double value = 0;
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      cubic_align_sh_coef_(&value, &Fs(0, tid), &zyz_(0, i), &stiff_[i]);
      *val += w_*value;
    }
    return 0;
  }
  int GraSH(const double *f, double *gra) const {
    itr_matrix<const double *> Fs(9, dim_/3, f);
    itr_matrix<double *> G(9, dim_/3, gra);
    matd_t g = zeros<double>(9, 1);
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      cubic_align_sh_coef_jac_(&g[0], &Fs(0, tid), &zyz_(0, i), &stiff_[i]);
      G(colon(), tid) += w_*g;
    }
    return 0;
  }
  int HesSH(const double *f, vector<Triplet<double>> *hes) const {
    matd_t H = zeros<double>(9, 9);
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      cubic_align_sh_coef_hes_(&H[0], nullptr, &zyz_(0, i), &stiff_[i]);
      for (size_t p = 0; p < 9; ++p) {
        for (size_t q = 0; q < 9; ++q) {
          const size_t I = 9*tid+p;
          const size_t J = 9*tid+q;
          hes->push_back(Triplet<double>(I, J, w_*H(p, q)));
        }
      }
    }
    return 0;
  }
private:
  const mati_t &tets_;
  const double w_;
  const size_t dim_;

  mati_t adjt_;
  matd_t stiff_;
  matd_t zyz_;
};

class poly_smooth_energy_tet : public SH_smooth_energy_tet
{
public:
  /*
   * About the magic number: f[I](s)=-2*sqrt(PI)/(15*sqrt(7))*(sqrt(7)Y40+sqrt(5)Y44)
   * As [Liu12] proves, SH = 16PI/315 poly, so magic = 20
   */
  poly_smooth_energy_tet(const mati_t &tets, const matd_t &nods, const double w)
      : SH_smooth_energy_tet(tets, nods, w), magic_(20.0) {
  }
  size_t Nx() const {
    return dim_;
  }
  int Val(const double *abc, double *val) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    matd_t abcs = zeros<double>(3, 2); double value = 0;
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      abcs = ABC(colon(), adjt_(colon(), i));
      poly_smooth_tet_(&value, &abcs[0], &stiff_[i]);
      *val += magic_*w_*value;
    }
    return 0;
  }
  int Gra(const double *abc, double *gra) const {
    itr_matrix<const double *> ABC(3, dim_/3, abc);
    itr_matrix<double *> G(3, dim_/3, gra);
    matd_t abcs = zeros<double>(3, 2), g = zeros<double>(3, 2);
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      abcs = ABC(colon(), adjt_(colon(), i));
      poly_smooth_tet_jac_(&g[0], &abcs[0], &stiff_[i]);
      G(colon(), adjt_(colon(), i)) += magic_*w_*g;
    }
    return 0;
  }
  int Hes(const double *abc, vector<Triplet<double>> *hes) const {
    return __LINE__;
  }
private:
  const double magic_;
};

class l1_smooth_energy_tet : public SH_smooth_energy_tet
{
public:
  l1_smooth_energy_tet(const mati_t &tets, const matd_t &nods, const double eps, const double w)
      : SH_smooth_energy_tet(tets, nods, w), epsilon_(eps) {
  }
  size_t Nx() const {
    return 3*dim_;
  }
  int Val(const double *f, double *val) const {
    itr_matrix<const double *> F(9, Nx()/9, f);
    matd_t frms = zeros<double>(9, 2); double value = 0;
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      frms = F(colon(), adjt_(colon(), i));
      l1_cubic_sym_smooth_(&value, &frms[0], &epsilon_, &stiff_[i]);
      *val += w_*value;
    }
    return 0;
  }
  int Gra(const double *f, double *gra) const {
    itr_matrix<const double *> F(9, Nx()/9, f);
    itr_matrix<double *> G(9, Nx()/9, gra);
    matd_t frms = zeros<double>(9, 2), g = zeros<double>(9, 2);
    for (size_t i = 0; i < adjt_.size(2); ++i) {
      frms = F(colon(), adjt_(colon(), i));
      l1_cubic_sym_smooth_jac_(&g[0], &frms[0], &epsilon_, &stiff_[i]);
      G(colon(), adjt_(colon(), i)) += w_*g;
    }
    return 0;
  }
  int Hes(const double *f, double *gra) const {
    return __LINE__;
  }
private:
  const double epsilon_;
};

class frame_orth_energy : public Functional<double>
{
public:
  frame_orth_energy(const mati_t &tets, const matd_t &nods, const double w)
      : tets_(tets), w_(w), dim_(9*tets.size(2)) {
    volume_.resize(tets.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < tets.size(2); ++i) {
        matd_t Ds = nods(colon(), tets(colon(1, 3), i))-nods(colon(), tets(0, i))*ones<double>(1, 3);
        volume_[i] = fabs(det(Ds))/6.0;
      }
      const double sum_volume = sum(volume_);
      volume_ /= sum_volume;
    }
  }
  size_t Nx() const {
    return dim_;
  }
  int Val(const double *f, double *val) const {
    double value = 0;
    for (size_t i = 0; i < tets_.size(2); ++i) {
      frm_orth_term_(&value, f+9*i, &volume_[i]);
      *val += w_*value;
    }
    return 0;
  }
  int Gra(const double *f, double *gra) const {
    itr_matrix<double *> G(9, dim_/9, gra);
    matd_t g = zeros<double>(9, 1);
    for (size_t i = 0; i < tets_.size(2); ++i) {
      frm_orth_term_jac_(&g[0], f+9*i, &volume_[i]);
      G(colon(), i) += w_*g;
    }
  }
  int Hes(const double *f, vector<Triplet<double>> *hes) const {
    return __LINE__;
  }
 private:
  const mati_t &tets_;
  const double w_;
  const size_t dim_;

  matd_t volume_;
};

class boundary_fix_energy : public Functional<double>
{
public:
  boundary_fix_energy(const mati_t &tets, const matd_t &nods, const VectorXd &x0,
                      const size_t var_dim, const double w)
      : tets_(tets), x0_(x0), var_dim_(var_dim), dim_(var_dim_*tets.size(2)), w_(w) {
    shared_ptr<face2tet_adjacent> f2t(face2tet_adjacent::create(tets));
    mati_t surf; bool check_order = true;
    jtf::mesh::get_outside_face(*f2t, surf, check_order, &nods);
  
    stiff_.resize(surf.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < surf.size(2); ++i)
        stiff_[i] = jtf::mesh::cal_face_area(nods(colon(), surf(colon(), i)));
      double sum_area = sum(stiff_);
      stiff_ /= sum_area;
    }
    
    adjt_.resize(surf.size(2)); {
      #pragma omp parallel for
      for (size_t i = 0; i < surf.size(2); ++i) {
        const pair<size_t, size_t> t = f2t->query(surf(0, i), surf(1, i), surf(2, i));
        adjt_[i] = (t.first == -1) ? t.second : t.first;
      }
    }
  }
  size_t Nx() const {
    return dim_;
  }
  int Val(const double *x, double *val) const {
    Map<const MatrixXd> X(x, var_dim_, dim_/var_dim_);
    Map<const MatrixXd> X0(x0_.data(), var_dim_, dim_/var_dim_);
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      *val += w_*stiff_[i]*(X.col(tid)-X0.col(tid)).squaredNorm();
    }
    return 0;
  }
  int Gra(const double *x, double *gra) const {
    Map<const MatrixXd> X(x, var_dim_, dim_/var_dim_);
    Map<const MatrixXd> X0(x0_.data(), var_dim_, dim_/var_dim_);
    Map<MatrixXd> G(gra, var_dim_, dim_/var_dim_);
    for (size_t i = 0; i < adjt_.size(); ++i) {
      const size_t tid = adjt_[i];
      G.col(tid) += w_*2.0*stiff_[i]*(X.col(tid)-X0.col(tid));
    }
    return 0;
  }
  int Hes(const double *x, vector<Triplet<double>> *hes) const {
    return __LINE__;
  }
private:
  const size_t var_dim_, dim_;
  const double w_;
  const mati_t &tets_;
  const VectorXd x0_;
  
  mati_t adjt_;
  matd_t stiff_;
};

//===============================================================================

cross_frame_opt::cross_frame_opt(const mati_t &tets, const matd_t &nods, const ptree &pt)
    : tets_(tets), nods_(nods), pt_(pt) {
  const double ws = pt_.get<double>("weight.smooth.value");
  const double wa = pt_.get<double>("weight.align.value");
  buffer_.push_back(make_shared<SH_smooth_energy_tet>(tets, nods, ws));
  buffer_.push_back(make_shared<SH_align_energy_tet>(tets, nods, wa));
}

int cross_frame_opt::solve_laplacian(VectorXd &Fs) const {
  ASSERT(buffer_.front().get());
  const size_t dim = 3*buffer_.front()->Nx();
  Fs = VectorXd::Zero(dim);
  cout << "\t@linear solve dimension: " << dim << endl << endl;

  auto fs = dynamic_pointer_cast<SH_smooth_energy_tet>(buffer_[0]);
  auto fa = dynamic_pointer_cast<SH_align_energy_tet>(buffer_[1]);

  double prev_vs = 0, prev_va = 0; {
    fs->ValSH(Fs.data(), &prev_vs);
    fa->ValSH(Fs.data(), &prev_va);
    cout << "\t@prev smoothness energy: " << prev_vs << endl;
    cout << "\t@prev alignment energy: " << prev_va << endl << endl;
  }

  VectorXd g = VectorXd::Zero(dim); {
    fs->GraSH(Fs.data(), g.data());
    fa->GraSH(Fs.data(), g.data());
    g *= -1;
    cout << "\t@prev grad norm: " << g.norm() << endl << endl;
  }
  
  SparseMatrix<double> H(dim, dim); {
    vector<Triplet<double>> trips;
    fs->HesSH(nullptr, &trips);
    fa->HesSH(nullptr, &trips);
    H.setFromTriplets(trips.begin(), trips.end());
    H.makeCompressed();
  }

  const string linear_solver = pt_.get<string>("lins.type.value", "PETSc");
  VectorXd dx = VectorXd::Zero(dim);
  if ( linear_solver == "PETSc" ) {
    static shared_ptr<PETsc_imp> petsc_init = make_shared<PETsc_imp>();
    shared_ptr<PETsc_CG_imp> solver =
        make_shared<PETsc_CG_imp>(H.valuePtr(), H.innerIndexPtr(), H.outerIndexPtr(), H.nonZeros(),
                                  dim, dim, "sor");
    solver->solve(g.data(), dx.data(), dim);
  } else {
    CholmodSimplicialLLT<SparseMatrix<double>> solver;
    solver.compute(H);
    ASSERT(solver.info() == Success);
    dx = solver.solve(g);
    ASSERT(solver.info() == Success);
  }
  
  Fs += dx;
  
  double post_vs = 0, post_va = 0; {
    fs->ValSH(Fs.data(), &post_vs);
    fa->ValSH(Fs.data(), &post_va);
    cout << "\t@post smoothness energy: " << post_vs << endl;
    cout << "\t@post alignment energy: " << post_va << endl << endl;
  }

  VectorXd gs = VectorXd::Zero(dim); {
    fs->GraSH(Fs.data(), gs.data());
    fa->GraSH(Fs.data(), gs.data());
    cout << "\t@post grad norm: " << gs.norm() << endl << endl;
  }

  cout << "\t@SOLUTION NORM: " << Fs.norm() << endl;
  return 0;
}

int cross_frame_opt::solve_initial_frames(const VectorXd &Fs, VectorXd &abc) const {
  ASSERT(buffer_.front().get());
  abc = VectorXd::Zero(buffer_.front()->Nx());
  
  #pragma omp parallel for
  for (size_t i = 0; i < abc.size()/3; ++i) {
    // double prev_res = 0; 
    // sh_residual_(&prev_res, &abc[3*i], &Fs[9*i]);
    
    sh_to_zyz(&Fs[9*i], &abc[3*i], 1000);

    // double post_res = 0;
    // sh_residual_(&post_res, &abc[3*i], &Fs[9*i]);
  }

  return 0;
}

int cross_frame_opt::optimize_frames(VectorXd &abc) const {
  const double epsf = pt_.get<double>("lbfgs.epsf.value"), epsx = 0;
  const size_t maxits = pt_.get<size_t>("lbfgs.maxits.value");
  
  shared_ptr<Functional<double>> energy_ = make_shared<energy_t<double>>(buffer_);
  
  {
    double vs = 0, va = 0;
    buffer_[0]->Val(abc.data(), &vs);
    buffer_[1]->Val(abc.data(), &va);
    cout << "\t@prev smoothness energy: " << vs << endl;
    cout << "\t@prev alignment energy: " << va << endl;
    VectorXd g = VectorXd::Zero(energy_->Nx());
    energy_->Gra(abc.data(), g.data());
    cout << "\t@prev gradient norm: " << g.norm() << endl << endl;
  }
  
  lbfgs_solve(energy_, abc.data(), abc.size(), epsf, epsx, maxits);

  {
    double vs = 0, va = 0;
    buffer_[0]->Val(abc.data(), &vs);
    buffer_[1]->Val(abc.data(), &va);
    cout << "\t@post smoothness energy: " << vs << endl;
    cout << "\t@post alignment energy: " << va << endl;
    VectorXd g = VectorXd::Zero(energy_->Nx());
    energy_->Gra(abc.data(), g.data());
    cout << "\t@post gradient norm: " << g.norm() << endl << endl;
  }

  return 0;
}

//===============================================================================

frame_smoother::frame_smoother(const mati_t &tets, const matd_t &nods, const ptree &pt)
    : tets_(tets), nods_(nods), pt_(pt) {
}

static shared_ptr<Functional<double>> g_func, g_smooth;
static size_t g_count;

static void callback_sh(const real_1d_array &x, double &func, real_1d_array &grad, void *ptr) {
  const size_t dim = g_func->Nx();
  const double *ptrx = x.getcontent();
  double *ptrg = grad.getcontent();

  func = 0;
  g_func->Val(ptrx, &func);

  std::fill(ptrg, ptrg+dim, 0);
  g_func->Gra(ptrx, ptrg);

  if ( g_count % 100 == 0 ) {
    VectorXd fmat;
    Map<const VectorXd> X(ptrx, dim);
    convert_zyz_to_mat(X, fmat);
    double value = 0;
    g_smooth->Val(fmat.data(), &value);
    cout << "\t # ITER " << g_count << ", " << value << endl;
  }

  ++g_count;
}

static void callback_l1(const real_1d_array &x, double &func, real_1d_array &grad, void *ptr) {
  const size_t dim = g_func->Nx();
  const double *ptrx = x.getcontent();
  double *ptrg = grad.getcontent();

  func = 0;
  g_func->Val(ptrx, &func);

  std::fill(ptrg, ptrg+dim, 0);
  g_func->Gra(ptrx, ptrg);

  if ( g_count % 100 == 0 ) {
    double value = 0;
    g_smooth->Val(ptrx, &value);
    cout << "\t # ITER " << g_count << ", " << value << endl;
  }

  ++g_count;
}

int frame_smoother::smoothSH(VectorXd &abc) const {
  ASSERT(abc.size() == 3*tets_.size(2));
  
  vector<shared_ptr<Functional<double>>> buffer(2);

  const double ws = pt_.get<double>("weight.smooth.value"),
      wp = pt_.get<double>("weight.boundary.value");
  buffer[0] = make_shared<SH_smooth_energy_tet>(tets_, nods_, ws);
  buffer[1] = make_shared<boundary_fix_energy>(tets_, nods_, abc, 3, wp);
  try {
    g_func = make_shared<energy_t<double>>(buffer);
  } catch ( ... ) {
    cerr << "[Error] SH energy exceptions!" << endl;
    exit(EXIT_FAILURE);
  }

  const double abs_eps = pt_.get<double>("abs_eps.value");
  g_smooth = make_shared<l1_smooth_energy_tet>(tets_, nods_, abs_eps, ws);

  g_count = 0;
   
  const double epsf = pt_.get<double>("lbfgs.epsf.value"), epsx = 0;
  const size_t maxits = pt_.get<size_t>("lbfgs.maxits.value");
  lbfgs_solve(callback_sh, abc.data(), abc.size(), epsf, epsx, maxits);

  return 0;
}

int frame_smoother::smoothL1(VectorXd &mat) const {
  ASSERT(mat.size() == 9*tets_.size(2));
  
  vector<shared_ptr<Functional<double>>> buffer(3);

  const double abs_eps = pt_.get<double>("abs_eps.value"),
      ws = pt_.get<double>("weight.smooth.value"),
      wo = pt_.get<double>("weight.orth.value"),
      wp = pt_.get<double>("weight.boundary.value");
  buffer[0] = make_shared<l1_smooth_energy_tet>(tets_, nods_, abs_eps, ws);
  buffer[1] = make_shared<frame_orth_energy>(tets_, nods_, wo);
  buffer[2] = make_shared<boundary_fix_energy>(tets_, nods_, mat, 9, wp);
  try {
    g_func = make_shared<energy_t<double>>(buffer);
  } catch ( ... ) {
    cerr << "[Error] L1 energy exceptions!\n";
    exit(EXIT_FAILURE);
  }

  g_smooth = buffer[0];

  g_count = 0;
  
  const double epsf = pt_.get<double>("lbfgs.epsf.value"), epsx = 0;
  const size_t maxits = pt_.get<size_t>("lbfgs.maxits.value");
  lbfgs_solve(callback_l1, mat.data(), mat.size(), epsf, epsx, maxits);

  // make frames orthogonal
#pragma omp parallel for
  for (size_t i = 0; i < mat.size()/9; ++i) {
    matd_t ff = itr_matrix<const double *>(3, 3, &mat[9*i]);
    matd_t U(3, 3), S(3, 3), VT(3, 3);
    svd(ff, U, S, VT);
    itr_matrix<double *>(3, 3, &mat[9*i]) = U*VT;
  }

  return 0;
}

}

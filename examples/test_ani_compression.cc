#include <iostream>
#include <boost/filesystem.hpp>
#include <jtflib/mesh/mesh.h>
#include <jtflib/mesh/io.h>

#include "src/json.h"
#include "igl/principal_curvature.h"
#include "igl/readOBJ.h"
#include "src/vtk.h"
#include "src/write_vtk.h"
#include "src/diffuse_dihedral_rot.h"
#include "src/dual_graph.h"

using namespace std;
using namespace Eigen;
using namespace riemann;
using namespace zjucad::matrix;

int main(int argc, char *argv[])
{
  if ( argc != 2 ) {
    cerr << "#usage: ./test_ani_compression config.json\n";
    return __LINE__;
  }
  Json::Reader reader;
  Json::Value json;
  ifstream ifs(argv[1]);
  if ( ifs.fail() ) {
    cerr << "[Error] can not open " << argv[1] << endl;
    return __LINE__;
  }
  if ( !reader.parse(ifs, json) ) {
    cerr << "[Error] " << reader.getFormattedErrorMessages() << endl;
    return __LINE__;
  }
  ifs.close();

  // INPUT
  mati_t tris; matd_t nods, nods_prev, nods_curr;
  jtf::mesh::load_obj(json["mesh_rest"].asString().c_str(), tris, nods);
  jtf::mesh::load_obj(json["mesh_prev"].asString().c_str(), tris, nods_prev);
  jtf::mesh::load_obj(json["mesh_curr"].asString().c_str(), tris, nods_curr);

  // BUILD SPANNING TREE OF DUAL GRAPH
  shared_ptr<edge2cell_adjacent> ec;
  shared_ptr<Graph> g;
  build_tri_mesh_dual_graph(tris, ec, g);
  tree_t mst;
  get_minimum_spanning_tree(g, mst);
  const size_t root_face = json["root_face"].asUInt();

  // ENCODE
  diffuse_arap_encoder encoder;
  vector<double> da;
  encoder.calc_delta_angle(tris, nods_prev, nods_curr, mst, root_face, da);

  // DECODE
  diffuse_arap_decoder decoder(tris, nods);
  decoder.estimate_rotation(nods_prev, mst, root_face, da);
//  for (int i = 0; i < json["handles"].size(); ++i) {
//    const size_t id = json["handles"][i]["id"].asUInt();
//    decoder.pin_down_vert(id, &nods_curr(0, id));
//  }
  for (size_t i = 0; i < 3; ++i) {
    const size_t id = tris(i, root_face);
    decoder.pin_down_vert(id, &nods_curr(0, id));
  }
  matd_t rec_curr(3, nods.size(2));
  decoder.solve(rec_curr);

  // OUTPUT
  {
    ofstream ofs(json["outdir"].asString()+"/tri_curr.vtk");
    tri2vtk(ofs, &nods_curr[0], nods_curr.size(2), &tris[0], tris.size(2));
    ofs.close();
  }
  {
    ofstream ofs(json["outdir"].asString()+"/tri_recover.vtk");
    tri2vtk(ofs, &rec_curr[0], rec_curr.size(2), &tris[0], tris.size(2));
    ofs.close();
  }

  cout << "[Info] done\n";
  return 0;
}

//int main(int argc, char *argv[])
//{
//  if ( argc != 2 ) {
//    cerr << "#usage: ./test_ani_compression config.json\n";
//    return __LINE__;
//  }
//  Json::Reader reader;
//  Json::Value json;
//  ifstream ifs(argv[1]);
//  if ( ifs.fail() ) {
//    cerr << "[Error] can not open " << argv[1] << endl;
//    return __LINE__;
//  }
//  if ( !reader.parse(ifs, json) ) {
//    cerr << "[Error] " << reader.getFormattedErrorMessages() << endl;
//    return __LINE__;
//  }
//  ifs.close();

//  Matrix<int, -1, -1, RowMajor> tris;
//  Matrix<double, -1, -1, RowMajor> nods;
//  igl::readOBJ(json["mesh"].asString(), nods, tris);
//  Matrix<double, -1, -1, RowMajor> pd1, pd2;
//  VectorXd pv1, pv2;
//  igl::principal_curvature(nods, tris, pd1, pd2, pv1, pv2);

//  double scale = json["scale"].asDouble();
//  Matrix<double, -1, -1, RowMajor> cd1 = scale*pv1.asDiagonal()*pd1;
//  Matrix<double, -1, -1, RowMajor> cd2 = scale*pv2.asDiagonakl()*pd2;

//  for (int i = 0; i < pv1.size(); ++i)
//    printf("node: %d, min: %lf, max: %lf\n", i, pv1(i), pv2(i));

//  string outdir = json["outdir"].asString();
//  boost::filesystem::create_directories(outdir);
//  char buffer[256];
//  sprintf(buffer, "%s/model.vtk", outdir.c_str());
//  ofstream ofs(buffer);
//  tri2vtk(ofs, nods.data(), nods.rows(), tris.data(), tris.rows());
//  ofs.close();
//  sprintf(buffer, "%s/lam_min.vtk", outdir.c_str());
//  draw_vert_direct_field(buffer, nods.data(), nods.rows(), cd1.data());
//  sprintf(buffer, "%s/lam_max.vtk", outdir.c_str());
//  draw_vert_direct_field(buffer, nods.data(), nods.rows(), cd2.data());

//  cout << "[Info] done\n";
//  return 0;
//}

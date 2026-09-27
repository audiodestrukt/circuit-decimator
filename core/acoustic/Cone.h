// Cone.h -- cone breakup: the paper cone as an axisymmetric shell (finite
// elements along its slope), driven at the neck by the voice coil and held at
// the edge by the surround. For each frequency it gives how the cone surface
// actually moves relative to the voice coil, which Radiation.h turns into the
// mic's response. (docs/briefs/2026-09-26-speaker-cab.md, step 4.)
//
// Shell of revolution, Kirchhoff-Love, straight-sided elements (a curved
// profile is a chain of them). Per element, with meridian angle a (the slope
// from the baffle plane), meridional displacement u, normal displacement w,
// radius r:
//
//   membrane:  e_s = du/ds            e_t = (u cos a - w sin a) / r
//   bending:   k_s = -d2w/ds2         k_t = -(cos a / r) dw/ds
//
//   U = 1/2 int [ Eh/(1-v^2) (e_s^2 + 2v e_s e_t + e_t^2)
//               + D (k_s^2 + 2v k_s k_t + k_t^2) ] 2 pi r ds,  D = E h^3 / 12(1-v^2)
//   T = 1/2 int rho h (u'^2 + w'^2) 2 pi r ds
//
// u linear, w cubic Hermite. Nodes carry GLOBAL (U_r, U_z, beta) so elements of
// different slope join correctly. Losses: hysteretic (complex modulus E(1+j eta)),
// plus the surround's dashpot.
//
// Boundary conditions:
//   neck (glued to the voice-coil former): U_z = 1, U_r = 0, beta = 0 (prescribed)
//   edge: surround springs (axial, radial) + axial dashpot + mass; rotation free
//   dust cap: a rigid dome glued at its radius, its mass lumped there (axial),
//             its stiffness restraining that ring (radial, rotation)
//
// Output: for each node, the cone's "piston-equivalent" velocity relative to
// the coil,   T = (U . n) / cos a = U_z - U_r tan a
// (normal velocity x true area, per unit projected area; 1 for a rigid cone).
// Air loading on the cone and non-axisymmetric modes are not modelled
// (a symmetric drive doesn't excite them; damage will).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace cd::acoustic {

using cplx = std::complex<double>;

// Cone geometry, shared by the shell model and the radiating surface.
// The cone runs from the voice coil (neck, depth below the edge) out to the
// effective radius; the dust cap is a dome glued on at dustCap.
struct Cone {
    double radius = 0.13;          // effective radiating radius (Sd = pi a^2 ~ 0.053 m^2 for a 12")
    double coilRadius = 0.022;     // neck: voice coil former radius (1.75" coil)
    double depth = 0.045;          // axial depth from the edge down to the neck
    double curve = 0.0;            // profile: 0 straight, > 0 curvilinear (deeper near the neck)
    double dustCap = 0.05;         // dust cap radius
    double capHeight = 0.015;      // dust cap dome height above where it's glued
};

// height (z, the baffle plane is 0) of the cone / dust cap surface at radius r
inline double coneHeight(const Cone& c, double r)
{
    auto cone = [&](double rr) {
        const double t = std::clamp((rr - c.coilRadius) / std::max(1e-9, c.radius - c.coilRadius), 0.0, 1.0);
        return -c.depth * (1 - t) * (1 + c.curve * t);
    };
    if (r >= c.dustCap) return cone(r);
    const double t = r / std::max(1e-9, c.dustCap);
    return cone(c.dustCap) + c.capHeight * (1 - t * t);
}

struct ConeMaterial {
    // provisional: plausible cone-paper values, not yet calibrated against real speakers
    double thickness = 0.6e-3;     // paper (m)
    double density = 500;          // kg/m^3
    double youngs = 4.0e9;         // Pa
    double poisson = 0.3;          // nu_s-theta
    // Paper cones are pressed with the fibres running mostly along the slope and are
    // thicker towards the neck; ribs stiffen bending along the slope.
    double anisotropy = 1.0;       // E_theta / E_s (around vs along the slope): < 1 = stiffer along the slope
    double ribs = 1.0;             // extra bending stiffness along the slope (x D_s): concentric ribs / corrugation
    double taper = 1.0;            // thickness at the neck / thickness at the edge (linear in between)
    double loss = 0.1;             // hysteretic loss factor of the (treated) paper
    double surroundK = 1700;       // axial stiffness at the edge (N/m): the surround's share of 1/Cms
    double surroundKr = 1e5;       // radial stiffness (N/m)
    double surroundR = 20;         // axial damping (N s/m): absorbs the bending waves arriving at the edge
    double surroundMass = 2e-3;    // kg
    double dustCapMass = 0.3e-3;   // kg, lumped where the dust cap is glued
    // A glued dome closes the ring it sits on: its membrane resists the ring
    // moving radially, its bending resists the ring rotating.
    double dustCapKr = 5e5;        // radial stiffness at the glue ring (N/m)
    double dustCapKrot = 5.0;      // rotational stiffness at the glue ring (N m / rad)
};

class ConeModel {
    // 4-point Gauss-Legendre on [0, 1]
    static constexpr double gaussX[4] = { 0.0694318442029737, 0.3300094782075719, 0.6699905217924281, 0.9305681557970263 };
    static constexpr double gaussW[4] = { 0.1739274225687269, 0.3260725774312731, 0.3260725774312731, 0.1739274225687269 };

public:
    // mesh: elements along the slope
    // Mesh node k of `elements`: two graded segments so a node lands exactly where the
    // dust cap is glued. Returns that node's index in capNode.
    static void meshNode(const Cone& p, int k, int elements, double& r, double& z, int& capNode)
    {
        const double span = p.radius - p.coilRadius;
        const double tc = std::clamp((p.dustCap - p.coilRadius) / span, 0.0, 1.0);
        int n1 = (int) std::lround(elements * tc);
        if (tc > 0 && tc < 1) n1 = std::clamp(n1, 1, elements - 1);
        const double t = k <= n1 ? (n1 > 0 ? tc * k / n1 : 0.0) : tc + (1 - tc) * (k - n1) / (elements - n1);
        r = p.coilRadius + t * span;
        z = -p.depth * (1 - t) * (1 + p.curve * t);   // coneHeight's cone part
        capNode = n1;
    }

    // The paper's mass, integrated exactly as assemble() does (same mesh, same
    // quadrature, no allocation): the circuit's moving mass is built from it.
    static double paperMass(const Cone& p, const ConeMaterial& m, int elements = 96)
    {
        double mass = 0, ra, za, rb, zb;
        int cap;
        const double span = std::max(1e-9, p.radius - p.coilRadius);
        meshNode(p, 0, elements, ra, za, cap);
        for (int e = 0; e < elements; ++e) {
            meshNode(p, e + 1, elements, rb, zb, cap);
            const double Le = std::hypot(rb - ra, zb - za);
            for (int g = 0; g < 4; ++g) {
                const double r = ra + gaussX[g] * (rb - ra);
                const double tr = std::clamp((r - p.coilRadius) / span, 0.0, 1.0);
                const double h = m.thickness * (m.taper + (1 - m.taper) * tr);
                mass += m.density * h * 2 * M_PI * r * gaussW[g] * Le;
            }
            ra = rb;
            za = zb;
        }
        return mass;
    }
    // everything the shell model moves as a rigid body: paper, surround, dust cap
    static double rigidMassOf(const Cone& p, const ConeMaterial& m, int elements = 96)
    {
        return paperMass(p, m, elements) + m.surroundMass + m.dustCapMass;
    }

    void build(const Cone& geom, const ConeMaterial& mat, int elements = 96)
    {
        p = geom;
        m = mat;
        const int ne = elements, nn = ne + 1;
        int n1 = 0;
        nodes.assign((size_t) nn, {});
        for (int k = 0; k < nn; ++k) meshNode(p, k, ne, nodes[(size_t) k].r, nodes[(size_t) k].z, n1);
        capNode = n1;
        assemble();
    }

    int numNodes() const { return (int) nodes.size(); }
    double nodeRadius(int k) const { return nodes[(size_t) k].r; }
    double nodeZ(int k) const { return nodes[(size_t) k].z; }
    int dustCapNode() const { return capNode; }
    double coneMass() const { return massTotal; }

    // Solve at angular frequency w: T (piston-equivalent velocity / coil velocity) per node.
    // `neck` is the prescribed neck motion (U_r, U_z, beta): the voice coil is (0, 1, 0);
    // tests drive it radially. The full nodal solution is kept (lastRadial/lastAxial).
    void solve(double w, std::vector<cplx>& T, std::array<cplx, 3> neck = { 0.0, 1.0, 0.0 }) const
    {
        const int nn = numNodes();   // node 0 prescribed, 1..nn-1 free
        // S = K (1 + j eta) - w^2 M + j w C, as 3x3 blocks: diag D[k], off-diagonal L[k] (k, k-1), U[k] (k, k+1)
        std::vector<Blk> D((size_t) nn), L((size_t) nn), U((size_t) nn);
        const cplx kfac(1, m.loss);
        auto dyn = [&](const std::array<double, 9>& k, const std::array<double, 9>& mm, const std::array<double, 9>& c) {
            Blk b;
            for (int i = 0; i < 9; ++i) b[(size_t) i] = k[(size_t) i] * kfac - w * w * mm[(size_t) i] + cplx(0, w * c[(size_t) i]);
            return b;
        };
        for (int k = 0; k < nn; ++k) {
            D[(size_t) k] = dyn(Kd[(size_t) k], Md[(size_t) k], Cd[(size_t) k]);
            for (int i = 0; i < 9; ++i) D[(size_t) k][(size_t) i] += Ks[(size_t) k][(size_t) i];   // springs: no paper loss
            if (k > 0) L[(size_t) k] = dyn(Kl[(size_t) k], Ml[(size_t) k], zero9());
            if (k + 1 < nn) U[(size_t) k] = dyn(Ku[(size_t) k], Mu[(size_t) k], zero9());
        }
        // prescribed neck: x0 = (0, 1, 0) -> rhs of node 1 = -L[1] x0
        std::vector<std::array<cplx, 3>> rhs((size_t) nn);
        const std::array<cplx, 3> x0 = neck;
        rhs[1] = mulv(L[1], x0);
        for (auto& v : rhs[1]) v = -v;
        // block Thomas on nodes 1..nn-1
        std::vector<Blk> G((size_t) nn);
        std::vector<std::array<cplx, 3>> y((size_t) nn);
        for (int k = 1; k < nn; ++k) {
            Blk a = D[(size_t) k];
            std::array<cplx, 3> b = rhs[(size_t) k];
            if (k > 1) {
                a = sub(a, mul(L[(size_t) k], G[(size_t) k - 1]));
                const auto t = mulv(L[(size_t) k], y[(size_t) k - 1]);
                for (int i = 0; i < 3; ++i) b[(size_t) i] -= t[(size_t) i];
            }
            const Blk ai = inv(a);
            if (k + 1 < nn) G[(size_t) k] = mul(ai, U[(size_t) k]);
            y[(size_t) k] = mulv(ai, b);
        }
        std::vector<std::array<cplx, 3>> x((size_t) nn);
        x[0] = x0;
        for (int k = nn - 1; k >= 1; --k) {
            x[(size_t) k] = y[(size_t) k];
            if (k + 1 < nn) {
                const auto t = mulv(G[(size_t) k], x[(size_t) k + 1]);
                for (int i = 0; i < 3; ++i) x[(size_t) k][(size_t) i] -= t[(size_t) i];
            }
        }
        T.resize((size_t) nn);
        for (int k = 0; k < nn; ++k) {
            // slope at the node: average of the adjacent elements
            const double ta = std::tan(nodeSlope(k));
            T[(size_t) k] = x[(size_t) k][1] - x[(size_t) k][0] * ta;
        }
        capAxial = x[(size_t) capNode][1];
        lastX = x;
        // force the coil applies to hold the neck on its prescribed motion: row 0 of S x
        const auto f0 = mulv(D[0], x[0]), f1 = mulv(U[0], x[1]);
        neckForce = f0[1] + f1[1];
    }
    cplx lastRadial(int k) const { return lastX[(size_t) k][0]; }
    // axial force at the neck per unit axial coil displacement at the last solve()
    cplx lastNeckForce() const { return neckForce; }
    // the moving parts the shell model carries (paper, surround, dust cap), as a rigid mass
    double rigidMass() const { return massTotal + m.surroundMass + m.dustCapMass; }
    const ConeMaterial& material() const { return m; }
    cplx lastAxial(int k) const { return lastX[(size_t) k][1]; }

    // T at radius r (linear between nodes); inside the dust cap, the cap's own motion
    cplx at(const std::vector<cplx>& T, double r) const
    {
        if (r < p.dustCap) return capAxial;
        const int nn = numNodes();
        if (r <= nodes[0].r) return T[0];
        for (int k = 1; k < nn; ++k)
            if (r <= nodes[(size_t) k].r) {
                const double f = (r - nodes[(size_t) k - 1].r) / (nodes[(size_t) k].r - nodes[(size_t) k - 1].r);
                return T[(size_t) k - 1] + f * (T[(size_t) k] - T[(size_t) k - 1]);
            }
        return T.back();
    }

    // the dust cap (rigid, glued at its node) moves with the cone's axial motion there
    cplx dustCapResponse() const { return capAxial; }

private:
    struct Node {
        double r, z;
    };
    Cone p;
    ConeMaterial m;
    std::vector<Node> nodes;
    int capNode = 0;
    double massTotal = 0;
    // assembled real matrices as 3x3 node blocks
    std::vector<std::array<double, 9>> Kd, Kl, Ku, Md, Ml, Mu, Cd;
    std::vector<std::array<double, 9>> Ks;   // surround + dust cap springs (their losses are elsewhere)
    mutable cplx capAxial = 1.0;
    mutable std::vector<std::array<cplx, 3>> lastX;
    mutable cplx neckForce = 0.0;

    static std::array<double, 9> zero9() { return {}; }

    double elemSlope(int e) const
    {
        const auto& a = nodes[(size_t) e];
        const auto& b = nodes[(size_t) e + 1];
        return std::atan2(b.z - a.z, b.r - a.r);
    }
    double nodeSlope(int k) const
    {
        const int ne = numNodes() - 1;
        if (k == 0) return elemSlope(0);
        if (k == ne) return elemSlope(ne - 1);
        return 0.5 * (elemSlope(k - 1) + elemSlope(k));
    }

    void assemble()
    {
        const int nn = numNodes(), ne = nn - 1;
        Kd.assign((size_t) nn, {}); Kl.assign((size_t) nn, {}); Ku.assign((size_t) nn, {});
        Md.assign((size_t) nn, {}); Ml.assign((size_t) nn, {}); Mu.assign((size_t) nn, {});
        Cd.assign((size_t) nn, {});
        Ks.assign((size_t) nn, {});
        massTotal = 0;
        // orthotropic: E_s along the slope, E_t = anisotropy E_s around it; nu_ts = nu E_t / E_s
        const double Es = m.youngs, Et = m.youngs * m.anisotropy, nu = m.poisson, nuT = nu * m.anisotropy;
        const double den = 1 - nu * nuT;
        const double span = std::max(1e-9, p.radius - p.coilRadius);
        const double* gx = gaussX;
        const double* gw = gaussW;
        for (int e = 0; e < ne; ++e) {
            const auto& na = nodes[(size_t) e];
            const auto& nb = nodes[(size_t) e + 1];
            const double Le = std::hypot(nb.r - na.r, nb.z - na.z);
            const double a = elemSlope(e), ca = std::cos(a), sa = std::sin(a);
            double ke[6][6] = {}, me[6][6] = {};   // local dofs: u_i, w_i, w'_i, u_j, w_j, w'_j
            for (int g = 0; g < 4; ++g) {
                const double xi = gx[g], wt = gw[g] * Le;
                const double r = na.r + xi * (nb.r - na.r);
                const double tr = std::clamp((r - p.coilRadius) / span, 0.0, 1.0);
                const double h = m.thickness * (m.taper + (1 - m.taper) * tr);   // taper x at the neck -> 1 at the edge
                const double A11 = Es * h / den, A22 = Et * h / den, A12 = nuT * Es * h / den;
                const double h3 = h * h * h / 12;
                const double D11 = Es * h3 / den * m.ribs, D22 = Et * h3 / den, D12 = nuT * Es * h3 / den;
                const double N[4] = { 1 - 3 * xi * xi + 2 * xi * xi * xi, Le * (xi - 2 * xi * xi + xi * xi * xi),
                                      3 * xi * xi - 2 * xi * xi * xi, Le * (-xi * xi + xi * xi * xi) };
                const double dN[4] = { (-6 * xi + 6 * xi * xi) / Le, (1 - 4 * xi + 3 * xi * xi),
                                       (6 * xi - 6 * xi * xi) / Le, (-2 * xi + 3 * xi * xi) };
                const double ddN[4] = { (-6 + 12 * xi) / (Le * Le), (-4 + 6 * xi) / Le,
                                        (6 - 12 * xi) / (Le * Le), (-2 + 6 * xi) / Le };
                const double Nu[2] = { 1 - xi, xi };
                // strain rows over the 6 local dofs
                double B[4][6] = {};
                B[0][0] = -1 / Le; B[0][3] = 1 / Le;                                        // e_s
                B[1][0] = ca * Nu[0] / r; B[1][3] = ca * Nu[1] / r;                          // e_t (u part)
                const int wi[4] = { 1, 2, 4, 5 };
                for (int q = 0; q < 4; ++q) {
                    B[1][wi[q]] = -sa * N[q] / r;                                            // e_t (w part)
                    B[2][wi[q]] = -ddN[q];                                                   // k_s
                    B[3][wi[q]] = -(ca / r) * dN[q];                                         // k_t
                }
                const double C[4][4] = { { A11, A12, 0, 0 }, { A12, A22, 0, 0 }, { 0, 0, D11, D12 }, { 0, 0, D12, D22 } };
                const double dA = 2 * M_PI * r * wt;
                for (int i = 0; i < 6; ++i)
                    for (int j = 0; j < 6; ++j) {
                        double s = 0;
                        for (int x = 0; x < 4; ++x)
                            for (int y = 0; y < 4; ++y) s += B[x][i] * C[x][y] * B[y][j];
                        ke[i][j] += s * dA;
                    }
                // mass: rho h (u^2 + w^2)
                double Nuv[6] = { Nu[0], 0, 0, Nu[1], 0, 0 }, Nwv[6] = { 0, N[0], N[1], 0, N[2], N[3] };
                for (int i = 0; i < 6; ++i)
                    for (int j = 0; j < 6; ++j) me[i][j] += m.density * h * (Nuv[i] * Nuv[j] + Nwv[i] * Nwv[j]) * dA;
                massTotal += m.density * h * dA;
            }
            // local (u, w, w') = R (U_r, U_z, beta):  u = U_r ca + U_z sa,  w = -U_r sa + U_z ca,  w' = beta
            double R[3][3] = { { ca, sa, 0 }, { -sa, ca, 0 }, { 0, 0, 1 } };
            double Tm[6][6] = {};
            for (int bI = 0; bI < 2; ++bI)
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) Tm[bI * 3 + i][bI * 3 + j] = R[i][j];
            auto transform = [&](double loc[6][6], double glob[6][6]) {
                double t1[6][6] = {};
                for (int i = 0; i < 6; ++i)
                    for (int j = 0; j < 6; ++j)
                        for (int k = 0; k < 6; ++k) t1[i][j] += loc[i][k] * Tm[k][j];
                for (int i = 0; i < 6; ++i)
                    for (int j = 0; j < 6; ++j) {
                        double s = 0;
                        for (int k = 0; k < 6; ++k) s += Tm[k][i] * t1[k][j];
                        glob[i][j] = s;
                    }
            };
            double kg[6][6], mg[6][6];
            transform(ke, kg);
            transform(me, mg);
            auto add = [&](std::vector<std::array<double, 9>>& blk, size_t node, double src[6][6], int bi, int bj) {
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) blk[node][(size_t) (i * 3 + j)] += src[bi * 3 + i][bj * 3 + j];
            };
            add(Kd, (size_t) e, kg, 0, 0); add(Kd, (size_t) e + 1, kg, 1, 1);
            add(Ku, (size_t) e, kg, 0, 1); add(Kl, (size_t) e + 1, kg, 1, 0);
            add(Md, (size_t) e, mg, 0, 0); add(Md, (size_t) e + 1, mg, 1, 1);
            add(Mu, (size_t) e, mg, 0, 1); add(Ml, (size_t) e + 1, mg, 1, 0);
        }
        // surround at the edge: axial + radial springs, axial dashpot, mass
        Ks.back()[0] += m.surroundKr;
        Ks.back()[4] += m.surroundK;
        Cd.back()[4] += m.surroundR;
        Md.back()[0] += m.surroundMass;
        Md.back()[4] += m.surroundMass;
        // dust cap: mass (axial) and the dome's restraint of the ring it's glued to
        Md[(size_t) capNode][4] += m.dustCapMass;
        Ks[(size_t) capNode][0] += m.dustCapKr;
        Ks[(size_t) capNode][8] += m.dustCapKrot;
    }

    // ---- 3x3 complex block helpers ----
    using Blk = std::array<cplx, 9>;
    static Blk mul(const Blk& a, const Blk& b)
    {
        Blk c {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) c[(size_t) (i * 3 + j)] += a[(size_t) (i * 3 + k)] * b[(size_t) (k * 3 + j)];
        return c;
    }
    static Blk sub(const Blk& a, const Blk& b)
    {
        Blk c;
        for (int i = 0; i < 9; ++i) c[(size_t) i] = a[(size_t) i] - b[(size_t) i];
        return c;
    }
    static std::array<cplx, 3> mulv(const Blk& a, const std::array<cplx, 3>& v)
    {
        std::array<cplx, 3> r {};
        for (int i = 0; i < 3; ++i)
            for (int k = 0; k < 3; ++k) r[(size_t) i] += a[(size_t) (i * 3 + k)] * v[(size_t) k];
        return r;
    }
    // Gauss-Jordan with partial pivoting
    static Blk inv(Blk a)
    {
        Blk r { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
        for (int c = 0; c < 3; ++c) {
            int piv = c;
            for (int i = c + 1; i < 3; ++i)
                if (std::abs(a[(size_t) (i * 3 + c)]) > std::abs(a[(size_t) (piv * 3 + c)])) piv = i;
            if (piv != c)
                for (int j = 0; j < 3; ++j) {
                    std::swap(a[(size_t) (c * 3 + j)], a[(size_t) (piv * 3 + j)]);
                    std::swap(r[(size_t) (c * 3 + j)], r[(size_t) (piv * 3 + j)]);
                }
            const cplx d = a[(size_t) (c * 3 + c)];
            for (int j = 0; j < 3; ++j) { a[(size_t) (c * 3 + j)] /= d; r[(size_t) (c * 3 + j)] /= d; }
            for (int i = 0; i < 3; ++i) {
                if (i == c) continue;
                const cplx f = a[(size_t) (i * 3 + c)];
                for (int j = 0; j < 3; ++j) {
                    a[(size_t) (i * 3 + j)] -= f * a[(size_t) (c * 3 + j)];
                    r[(size_t) (i * 3 + j)] -= f * r[(size_t) (c * 3 + j)];
                }
            }
        }
        return r;
    }
};

} // namespace cd::acoustic

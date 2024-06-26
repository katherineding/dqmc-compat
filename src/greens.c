#include "greens.h"
#include "linalg.h"
#include "prof.h"
#include "mem.h"

void mul_seq(const int N,
		const int min, const int maxp1,
		const num alpha, num *const *const B, const int ldB,
		num *const A, const int ldA,
		num *const tmpNN) // assume tmpNN has ldB leading dim
{
	const int n_mul = maxp1 - min;
	if (n_mul <= 0)
		return;
	if (n_mul == 1) {
		for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			A[i + ldA*j] = alpha*B[min][i + ldB*j];
		return;
	}

	int l = min;
	if (n_mul % 2 == 0) {
		xgemm("N", "N", N, N, N, alpha, B[l + 1],
		      ldB, B[l], ldB, 0.0, A, ldA);
		l += 2;
	} else {
		xgemm("N", "N", N, N, N, alpha, B[l + 1],
		      ldB, B[l], ldB, 0.0, tmpNN, ldB);
		xgemm("N", "N", N, N, N, 1.0, B[l + 2],
		      ldB, tmpNN, ldB, 0.0, A, ldA);
		l += 3;
	}

	for (; l != maxp1; l += 2) {
		xgemm("N", "N", N, N, N, 1.0, B[l],
		      ldB, A, ldA, 0.0, tmpNN, ldB);
		xgemm("N", "N", N, N, N, 1.0, B[l + 1],
		      ldB, tmpNN, ldB, 0.0, A, ldA);
	}
}

int get_lwork_eq_g(const int N, const int ld)
{
	num lwork;
	int info = 0;
	int max_lwork = 0;

	xgeqp3(N, N, NULL, ld, NULL, NULL, &lwork, -1, NULL, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xgeqrf(N, N, NULL, ld, NULL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("R", "N", N, N, N, NULL, ld, NULL, NULL, ld, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("R", "C", N, N, N, NULL, ld, NULL, NULL, ld, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("L", "N", N, N, N, NULL, ld, NULL, NULL, ld, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	return max_lwork;
}

void calc_QdX_first(
		const int trans, // if 1, calculate QdX of B^T (conjugate transpose for complex)
		const int N, const int ld,
		const num *const B, // input
		const struct QdX *const QdX, // output
		num *const tmpN, // work arrays
		int *const pvt,
		num *const work, const int lwork)
{
	__builtin_assume(ld % MEM_ALIGN_NUM == 0);
	(void)__builtin_assume_aligned(B, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpN, MEM_ALIGN);
	(void)__builtin_assume_aligned(work, MEM_ALIGN);
	num *const Q = __builtin_assume_aligned(QdX->Q, MEM_ALIGN);
	num *const tau = __builtin_assume_aligned(QdX->tau, MEM_ALIGN);
	num *const d = __builtin_assume_aligned(QdX->d, MEM_ALIGN);
	num *const X = __builtin_assume_aligned(QdX->X, MEM_ALIGN);

	int info = 0;
	for (int i = 0; i < N; i++) pvt[i] = 0;
	xomatcopy('C', trans ? 'C' : 'N', N, N, 1.0, B, ld, Q, ld);

	// use d as RWORK for zgeqp3
	xgeqp3(N, N, Q, ld, pvt, tau, work, lwork, (double *)d, &info);

	for (int i = 0; i < N; i++) {
		d[i] = Q[i + i*ld];
		if (d[i] == 0.0) d[i] = 1.0;
		tmpN[i] = 1.0/d[i];
	}

	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			X[i + j*ld] = 0.0;

	for (int j = 0; j < N; j++)
		for (int i = 0; i <= j; i++)
			X[i + (pvt[j]-1)*ld] = tmpN[i] * Q[i + j*ld];
}

void calc_QdX(
		const int trans, // if 1, calculate QdX of B^T (conjugate transpose for complex)
		const int N, const int ld,
		const num *const B, // input
		const struct QdX *const QdX_prev,  // input, previous QdX
		const struct QdX *const QdX,  // output
		num *const tmpN, // work arrays
		int *const pvt,
		num *const work, const int lwork)
{
	__builtin_assume(ld % MEM_ALIGN_NUM == 0);
	(void)__builtin_assume_aligned(B, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpN, MEM_ALIGN);
	(void)__builtin_assume_aligned(work, MEM_ALIGN);
	const num *const prevQ = __builtin_assume_aligned(QdX_prev->Q, MEM_ALIGN);
	const num *const prevtau = __builtin_assume_aligned(QdX_prev->tau, MEM_ALIGN);
	const num *const prevd = __builtin_assume_aligned(QdX_prev->d, MEM_ALIGN);
	const num *const prevX = __builtin_assume_aligned(QdX_prev->X, MEM_ALIGN);
	num *const Q = __builtin_assume_aligned(QdX->Q, MEM_ALIGN);
	num *const tau = __builtin_assume_aligned(QdX->tau, MEM_ALIGN);
	num *const d = __builtin_assume_aligned(QdX->d, MEM_ALIGN);
	num *const X = __builtin_assume_aligned(QdX->X, MEM_ALIGN);

	// use X as temp N*N storage
	int info = 0;
	xomatcopy('C', trans ? 'C' : 'N', N, N, 1.0, B, ld, X, ld);

	xunmqr("R", "N", N, N, N, prevQ, ld, prevtau, X, ld, work, lwork, &info);
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			X[i + j*ld] *= prevd[j];

	for (int j = 0; j < N; j++) { // use tmpN for norms
		tmpN[j] = 0.0;
		for (int i = 0; i < N; i++)
			tmpN[j] += X[i + j*ld] * conj(X[i + j*ld]);
	}

	pvt[0] = 0;
	for (int i = 1; i < N; i++) { // insertion sort
		int j;
		for (j = i; j > 0 && creal(tmpN[pvt[j-1]]) < creal(tmpN[i]); j--)
			pvt[j] = pvt[j-1];
		pvt[j] = i;
	}

	for (int j = 0; j < N; j++) // pre-pivot
		my_copy(Q + j*ld, X + pvt[j]*ld, N);

	xgeqrf(N, N, Q, ld, tau, work, lwork, &info);

	for (int i = 0; i < N; i++) {
		d[i] = Q[i + i*ld];
		if (d[i] == 0.0) d[i] = 1.0;
		tmpN[i] = 1.0/d[i];
	}

	for (int j = 0; j < N; j++)
		for (int i = 0; i <= j; i++)
			Q[i + j*ld] *= tmpN[i];

	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			X[i + j*ld] = prevX[pvt[i] + j*ld];

	xtrmm("L", "U", "N", "N", N, N, 1.0, Q, ld, X, ld);
}

num calc_Gtt_last(
		const int trans, // if 0 calculate, calculate G = (1 + Q d X)^-1. if 1, calculate G = (1 + X.T d Q.T)^-1
		const int N, const int ld,
		const struct QdX *const QdX, // input
		num *const G, // output
		num *const tmpNN, // work arrays
		num *const tmpN,
		int *const pvt,
		num *const work, const int lwork)
{
	__builtin_assume(ld % MEM_ALIGN_NUM == 0);
	(void)__builtin_assume_aligned(G, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpNN, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpN, MEM_ALIGN);
	(void)__builtin_assume_aligned(work, MEM_ALIGN);
	const num *const Q = __builtin_assume_aligned(QdX->Q, MEM_ALIGN);
	const num *const tau = __builtin_assume_aligned(QdX->tau, MEM_ALIGN);
	const num *const d = __builtin_assume_aligned(QdX->d, MEM_ALIGN);
	const num *const X = __builtin_assume_aligned(QdX->X, MEM_ALIGN);

	int info = 0;

	// construct g from Eq 2.12 of 10.1016/j.laa.2010.06.023
//todo try double d = 1.0/d[i];
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			G[i + j*ld] = 0.0;
	for (int i = 0; i < N; i++) {
		if (fabs(d[i]) > 1.0) { // tmpN = 1/Db; tmpNN = Ds X
			tmpN[i] = 1.0/d[i];
			for (int j = 0; j < N; j++)
				tmpNN[i + j*ld] = X[i + j*ld];
		} else {
			tmpN[i] = 1.0;
			for (int j = 0; j < N; j++)
				tmpNN[i + j*ld] = d[i] * X[i + j*ld];
		}
		G[i + i*ld] = tmpN[i];
	}

	xunmqr("R", "C", N, N, N, Q, ld, tau, G, ld, work, lwork, &info);

	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			tmpNN[i + j*ld] += G[i + j*ld];

	xgetrf(N, N, tmpNN, ld, pvt, &info);
	xgetrs("N", N, N, tmpNN, ld, pvt, G, ld, &info);

	if (trans)
		ximatcopy('C', 'C', N, N, 1.0, G, ld, ld);

	// probably can be done more efficiently but it's O(N) so whatev
#ifdef USE_CPLX
	num phase = 1.0;
	for (int i = 0; i < N; i++) {
		const num c = tmpNN[i + i*ld]/tmpN[i];
		phase *= c/cabs(c);
		double vv = 1.0;
		for (int j = i + 1; j < N; j++)
			vv += creal(Q[j + i*ld])*creal(Q[j + i*ld])
			    + cimag(Q[j + i*ld])*cimag(Q[j + i*ld]);
		const num ref = 1.0 - tau[i]*vv;
		phase *= ref/cabs(ref);
		if (pvt[i] != i+1) phase *= -1.0;
	}
	return phase;
#else
	int sign = 1.0;
	for (int i = 0; i < N; i++) 
		if ((tmpN[i] < 0) ^ (tau[i] > 0) ^ (pvt[i] != i+1) ^ (tmpNN[i + i*ld] < 0))
			sign *= -1;
	return (double)sign;
#endif
}

// G = (1 + Q0 d0 X0 X1.T d1 Q1.T)^-1
//   = Q1 id1b (id0b Q0.T Q1 id1b + d0s X0 X1.T d1s)^-1 id0b Q0.T
// step by step:
// 1. tmpNN0 = d0s X0 X1.T d1s
// 2. G = id0b Q0.T
// 3. tmpNN1 = G Q1 id1b
// 4. tmpNN1 += tmpNN0
// 5. G = tmpNN1^-1 G
// 6. G = Q1 id1b G
num calc_Gtt(
		const int N, const int ld,
		const struct QdX *const QdX0, // input
		const struct QdX *const QdX1, // input
		num *const G, // output
		num *const tmpNN0, // work arrays
		num *const tmpNN1,
		num *const tmpN0,
		num *const tmpN1,
		int *const pvt,
		num *const work, const int lwork)
{
	__builtin_assume(ld % MEM_ALIGN_NUM == 0);
	(void)__builtin_assume_aligned(G, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpNN0, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpNN1, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpN0, MEM_ALIGN);
	(void)__builtin_assume_aligned(tmpN1, MEM_ALIGN);
	(void)__builtin_assume_aligned(work, MEM_ALIGN);
	const num *const Q0 = __builtin_assume_aligned(QdX0->Q, MEM_ALIGN);
	const num *const tau0 = __builtin_assume_aligned(QdX0->tau, MEM_ALIGN);
	const num *const d0 = __builtin_assume_aligned(QdX0->d, MEM_ALIGN);
	const num *const X0 = __builtin_assume_aligned(QdX0->X, MEM_ALIGN);
	const num *const Q1 = __builtin_assume_aligned(QdX1->Q, MEM_ALIGN);
	const num *const tau1 = __builtin_assume_aligned(QdX1->tau, MEM_ALIGN);
	const num *const d1 = __builtin_assume_aligned(QdX1->d, MEM_ALIGN);
	const num *const X1 = __builtin_assume_aligned(QdX1->X, MEM_ALIGN);

	int info = 0;

	// 1. tmpNN0 = d0s X0 X1.T d1s
	// tmpNN0 = X0 X1.T
	xgemm("N", "C", N, N, N, 1.0, X0, ld, X1, ld, 0.0, tmpNN0, ld);
	// tmpN1 = d0s, tmpN0 = id0b
	for (int i = 0; i < N; i++) {
		if (fabs(d0[i]) > 1.0) {
			tmpN0[i] = 1.0/d0[i];
			tmpN1[i] = 1.0;
		} else {
			tmpN0[i] = 1.0;
			tmpN1[i] = d0[i];
		}
	}
	// tmpNN0 = d0s tmpNN0 d1s
	for (int j = 0; j < N; j++) {
		const num d1s = fabs(d1[j]) > 1.0 ? 1.0 : d1[j];
		for (int i = 0; i < N; i++)
			tmpNN0[i + j*ld] *= tmpN1[i] * d1s;
	}

	// 2. G = id0b Q0.T
	// G = id0b
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			G[i + j*ld] = 0.0;
	for (int i = 0; i < N; i++) G[i + i*ld] = tmpN0[i];
	// G = G Q0.T
	xunmqr("R", "C", N, N, N, Q0, ld, tau0, G, ld, work, lwork, &info);

	// 3. tmpNN1 = G Q1 id1b
	// tmpNN1 = G Q1
	xomatcopy('C', 'N', N, N, 1.0, G, ld, tmpNN1, ld);
	xunmqr("R", "N", N, N, N, Q1, ld, tau1, tmpNN1, ld, work, lwork, &info);
	// tmpN1 = id1b
	for (int i = 0; i < N; i++) {
		if (fabs(d1[i]) > 1.0)
			tmpN1[i] = 1.0/d1[i];
		else
			tmpN1[i] = 1.0;
	}

	// combine last part of 3 with 4. tmpNN1 += tmpNN0
	// tmpNN1 = tmpNN1 tmpN1 + tmpNN0
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			tmpNN1[i + j*ld] = tmpNN1[i + j*ld]*tmpN1[j] + tmpNN0[i + j*ld];

	// 5. G = tmpNN1^-1 G
	xgetrf(N, N, tmpNN1, ld, pvt, &info);
	xgetrs("N", N, N, tmpNN1, ld, pvt, G, ld, &info);

	// 6. G = Q1 id1b G
	// G = id1b G
	for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			G[i + j*ld] *= tmpN1[i];
	// G = Q1 G
	xunmqr("L", "N", N, N, N, Q1, ld, tau1, G, ld, work, lwork, &info);

#ifdef USE_CPLX
	num phase = 1.0;
	for (int i = 0; i < N; i++) {
		const num c = tmpNN1[i + i*ld]/(tmpN0[i] * tmpN1[i]);
		phase *= c/cabs(c);

		double x0 = 1.0, x1 = 1.0;
		for (int j = i + 1; j < N; j++) {
			x0 += creal(Q0[j + i*ld])*creal(Q0[j + i*ld])
			    + cimag(Q0[j + i*ld])*cimag(Q0[j + i*ld]);
			x1 += creal(Q1[j + i*ld])*creal(Q1[j + i*ld])
			    + cimag(Q1[j + i*ld])*cimag(Q1[j + i*ld]);
		}
		const num refs = (1.0 - tau0[i]*x0)/(1.0 - tau1[i]*x1);
		phase *= refs/cabs(refs);

		if (pvt[i] != i+1) phase *= -1.0;
	}
	return phase;
#else
	int sign = 1.0;
	for (int i = 0; i < N; i++) 
		if ((tmpNN1[i + i*ld] < 0) ^ (tmpN0[i] < 0) ^ (tmpN1[i] < 0) ^
				(tau0[i] > 0) ^ (tau1[i] > 0) ^ (pvt[i] != i+1))
			sign *= -1;
	return (double)sign;
#endif
}

int get_lwork_ue_g(const int N, const int L)
{
	num lwork;
	int info = 0;
	int max_lwork = 0;

	if (L == 1) {  // then bsofi doesn't use QR
		xgetri(N, NULL, N, NULL, &lwork, -1, &info);
		if (creal(lwork) > max_lwork) max_lwork = (int)lwork;
		return max_lwork;
	}

	const int NL = N*L;
	const int N2 = 2*N;

	xgeqrf(N2, N, NULL, NL, NULL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("L", "C", N2, N, N, NULL, NL, NULL, NULL, NL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xgeqrf(N2, N2, NULL, NL, NULL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("R", "C", NL, N2, N2, NULL, N2, NULL, NULL, NL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	xunmqr("R", "C", NL, N2, N, NULL, N2, NULL, NULL, NL, &lwork, -1, &info);
	if (creal(lwork) > max_lwork) max_lwork = (int)lwork;

	return max_lwork;
}

static void calc_o(const int N, const int ld, const int L, const int n_mul,
		num *const *const B, num *const G,
		num *const tmpNN)
{
	const int E = 1 + (L - 1) / n_mul;
	const int NE = N*E;

	for (int i = 0; i < NE * NE; i++) G[i] = 0.0;

	for (int e = 0; e < E - 1; e++) // subdiagonal blocks
		mul_seq(N, e*n_mul, (e + 1)*n_mul, -1.0, B, ld,
		        G + N*(e + 1) + NE*N*e, NE, tmpNN);

	mul_seq(N, (E - 1)*n_mul, L, 1.0, B, ld, // top right corner
		G + NE*N*(E - 1), NE, tmpNN);

	for (int i = 0; i < NE; i++) G[i + NE*i] += 1.0; // 1 on diagonal
}

static void bsofi(const int N, const int L,
		num *const G, // input: O matrix, output: G = O^-1
		num *const tau, // NL
		num *const Q, // 2*N * 2*N
		num *const work, const int lwork)
{
	int info;

	if (L == 1) {
		xgetrf(N, N, G, N, (int *)tau, &info);
		xgetri(N, G, N, (int *)tau, work, lwork, &info);
		return;
	}

	const int NL = N*L;
	const int N2 = 2*N;

	#define G_BLK(i, j) (G + N*(i) + NL*N*(j))
	// block qr
	for (int l = 0; l < L - 2; l++) {
		xgeqrf(N2, N, G_BLK(l, l), NL, tau + N*l, work, lwork, &info);
		xunmqr("L", "C", N2, N, N, G_BLK(l, l), NL, tau + N*l,
		       G_BLK(l, l + 1), NL, work, lwork, &info);
		xunmqr("L", "C", N2, N, N, G_BLK(l, l), NL, tau + N*l,
		       G_BLK(l, L - 1), NL, work, lwork, &info);
	}
	xgeqrf(N2, N2, G_BLK(L - 2, L - 2), NL, tau + N*(L - 2), work, lwork, &info);

	// invert r
	if (L <= 2) {
		xtrtri("U", "N", NL, G, NL, &info);
	} else {
		xtrtri("U", "N", 3*N, G_BLK(L - 3, L - 3), NL, &info);
		if (L > 3) {
			xtrmm("R", "U", "N", "N", N*(L - 3), N, 1.0,
			      G_BLK(L - 1, L - 1), NL, G_BLK(0, L - 1), NL);
			for (int l = L - 4; l >= 0; l--) {
				xtrtri("U", "N", N, G_BLK(l, l), NL, &info);
				xtrmm("L", "U", "N", "N", N, N, -1.0,
				      G_BLK(l, l), NL, G_BLK(l, L - 1), NL);
				xtrmm("L", "U", "N", "N", N, N, -1.0,
				      G_BLK(l, l), NL, G_BLK(l, l + 1), NL);
				xgemm("N", "N", N, N*(L - l - 2), N, 1.0,
				      G_BLK(l, l + 1), NL, G_BLK(l + 1, l + 2), NL, 1.0,
				      G_BLK(l, l + 2), NL);
				xtrmm("R", "U", "N", "N", N, N, 1.0,
				      G_BLK(l + 1, l + 1), NL, G_BLK(l, l + 1), NL);
			}
		}
	}

	// multiply by q inverse
	for (int i = 0; i < 4*N*N; i++) Q[i] = 0.0;

	for (int j = 0; j < N2; j++)
	for (int i = j + 1; i < N2; i++) {
		Q[i + N2*j] = G_BLK(L - 2, L - 2)[i + NL*j];
		G_BLK(L - 2, L - 2)[i + NL*j] = 0.0;
	}
	xunmqr("R", "C", NL, N2, N2, Q, N2, tau + N*(L - 2),
	       G_BLK(0, L - 2), NL, work, lwork, &info);
	for (int l = L - 3; l >= 0; l--) {
		for (int j = 0; j < N; j++)
		for (int i = j + 1; i < N2; i++) {
			Q[i + N2*j] = G_BLK(l, l)[i + NL*j];
			G_BLK(l, l)[i + NL*j] = 0.0;
		}
		xunmqr("R", "C", NL, N2, N, Q, N2, tau + N*l,
		       G_BLK(0, l), NL, work, lwork, &info);
	}
	#undef G_BLK
}

static void expand_g(const int N, const int ld, const int L, const int E, const int n_matmul,
		num *const *const B,
		num *const *const iB,
		const num *const Gred,
		num *const G0t, num *const Gtt, num *const Gt0)
{
	// number of steps to move in each direction
	// except for boundaries, when L % n_matmul != 0
	const int n_left = (n_matmul - 1)/2;
	const int n_right = n_matmul/2;
	const int n_up = n_left;
	const int n_down = n_right;

	const int rstop_last = ((E - 1)*n_matmul + L)/2;
	const int lstop_first = (rstop_last + 1) % L;
	const int dstop_last = rstop_last;
	const int ustop_first = lstop_first;

	// copy Gred to G0t
	for (int f = 0; f < E; f++) {
		const int t = f*n_matmul;
		for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			G0t[i + ld*j + ld*N*t] = Gred[i + N*E*(j + N*f)];
	}

	// expand G0t
	for (int f = 0; f < E; f++) {
		const int l = f*n_matmul;
		const int lstop = (f == 0) ? lstop_first : l - n_left;
		const int rstop = (f == E - 1) ? rstop_last : l + n_right;
		for (int m = l; m != lstop;) {
			const int next = (m - 1 + L) % L;
			const num alpha = (m == 0) ? -1.0 : 1.0;
			xgemm("N", "N", N, N, N, alpha,
			      G0t + ld*N*m, ld, B[next], ld, 0.0,
			      G0t + ld*N*next, ld);
			m = next;
		}
		for (int m = l; m != rstop;) {
			const int next = (m + 1) % L;
			const num alpha = (next == 0) ? -1.0 : 1.0;
			const num beta = (m == 0) ? -alpha : 0.0;
			if (m == 0)
				for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++)
					G0t[i + ld*j + ld*N*next] = iB[m][i + ld*j];
			xgemm("N", "N", N, N, N, alpha,
			      G0t + ld*N*m, ld, iB[m], ld, beta,
			      G0t + ld*N*next, ld);
			m = next;
		}
	}


	// copy Gred to Gtt
	for (int e = 0; e < E; e++) {
		const int k = e*n_matmul;
		for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			Gtt[i + ld*j + ld*N*k] = Gred[(i + N*e) + N*E*(j + N*e)];
	}

	// expand Gtt
	// possible to save 2 gemm's here by using Gt0 and G0t but whatever
	for (int e = 0; e < E; e++) {
		const int k = e*n_matmul;
		const int ustop = (e == 0) ? ustop_first : k - n_up;
		const int dstop = (e == E - 1) ? dstop_last : k + n_down;
		for (int m = k; m != ustop;) {
			const int next = (m - 1 + L) % L;
			xgemm("N", "N", N, N, N, 1.0,
			      Gtt + ld*N*m, ld, B[next], ld, 0.0,
			      Gt0, ld); // use Gt0 as temporary
			xgemm("N", "N", N, N, N, 1.0,
			      iB[next], ld, Gt0, ld, 0.0,
			      Gtt + ld*N*next, ld);
			m = next;
		}
		for (int m = k; m != dstop;) {
			const int next = (m + 1) % L;
			xgemm("N", "N", N, N, N, 1.0,
			      Gtt + ld*N*m, ld, iB[m], ld, 0.0,
			      Gt0, ld);
			xgemm("N", "N", N, N, N, 1.0,
			      B[m], ld, Gt0, ld, 0.0,
			      Gtt + ld*N*next, ld);
			m = next;
		}
	}

	// copy Gred to Gt0
	for (int e = 0; e < E; e++) {
		const int t = e*n_matmul;
		for (int j = 0; j < N; j++)
		for (int i = 0; i < N; i++)
			Gt0[i + ld*j + ld*N*t] = Gred[(i + N*e) + N*E*j];
	}

	// expand Gt0
	for (int e = 0; e < E; e++) {
		const int k = e*n_matmul;
		const int ustop = (e == 0) ? ustop_first : k - n_up;
		const int dstop = (e == E - 1) ? dstop_last : k + n_down;
		for (int m = k; m != ustop;) {
			const int next = (m - 1 + L) % L;
			const num alpha = (m == 0) ? -1.0 : 1.0;
			const num beta = (m == 0) ? -alpha : 0.0;
			if (m == 0)
				for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++)
					Gt0[i + ld*j + ld*N*next] = iB[next][i + ld*j];
			xgemm("N", "N", N, N, N, alpha,
			      iB[next], ld, Gt0 + ld*N*m, ld, beta,
			      Gt0 + ld*N*next, ld);
			m = next;
		}
		for (int m = k; m != dstop;) {
			const int next = (m + 1) % L;
			const num alpha = (next == 0) ? -1.0 : 1.0;
			xgemm("N", "N", N, N, N, alpha,
			      B[m], ld, Gt0 + ld*N*m, ld, 0.0,
			      Gt0 + ld*N*next, ld);
			if (next == 0) // should never happen
				for (int i = 0; i < N; i++)
					Gt0[i + ld*i + ld*N*next] += 1.0;
			m = next;
		}
	}
}

void calc_ue_g(const int N, const int ld, const int L, const int F, const int n_mul,
		num *const *const B,
		num *const *const iB,
		num *const *const C,
		num *const G0t, num *const Gtt,
		num *const Gt0,
		num *const Gred,
		num *const tau,
		num *const Q,
		num *const work, const int lwork)
{
	const int E = 1 + (F - 1) / n_mul;

	profile_begin(calc_o);
	calc_o(N, ld, F, n_mul, C, Gred, Q); // use Q as tmpNN
	profile_end(calc_o);

	profile_begin(bsofi);
	bsofi(N, E, Gred, tau, Q, work, lwork);
	profile_end(bsofi);

	profile_begin(expand_g);
	expand_g(N, ld, L, E, (L/F) * n_mul, B, iB, Gred, G0t, Gtt, Gt0);
	profile_end(expand_g);
}

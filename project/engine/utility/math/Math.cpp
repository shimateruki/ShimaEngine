#include "Math.h"
#include <numbers>
#include <cstdint>
#include <filesystem>
#include <string>
#include <cmath> // C++ の数学ライブラリ
#include <algorithm> // std::max, std::min用

namespace engine {


// 位置の差を取り、始点v2から終点v1への変位を返す。
Vector3 operator-(const Vector3& v1, const Vector3& v2) {
	return { v1.x - v2.x, v1.y - v2.y, v1.z - v2.z };
}

Vector3 operator*(const Vector3& v, float scalar) {
	return { v.x * scalar, v.y * scalar, v.z * scalar };
}

Vector3 operator+(const Vector3& v1, const Vector3& v2) {

	return { v1.x + v2.x, v1.y + v2.y, v1.z + v2.z };

}

// 各成分を同じ分母で割る。分母が0ならゼロベクトル。
Vector3 operator/(const Vector3& v, float scalar) {
	// ゼロ以外の分母だけ除算に使う。
	if (scalar != 0.0f) {
		return { v.x / scalar, v.y / scalar, v.z / scalar };
	}

	// 分母が0の場合はゼロベクトルを返し、ゼロ除算による非有限値を避ける。
	return { 0.0f, 0.0f, 0.0f };
}

Vector3 operator-(const Vector3& v) {
	return { -v.x, -v.y, -v.z };
}

// 各要素は左の行と右の列の内積です。点を左側に置くため、変換も左から順に適用します。
Matrix4x4 operator*(const Matrix4x4& m1, const Matrix4x4& m2) {
	Matrix4x4 result = {};

	for (std::int32_t i = 0; i < 4; ++i) {
		for (std::int32_t j = 0; j < 4; ++j) {
			result.m[i][j] = 0;

			for (std::int32_t k = 0; k < 4; ++k) {
				result.m[i][j] += m1.m[i][k] * m2.m[k][j];
			}
		}
	}

	return result;
}

// 左のベクトル自体を更新する。返す参照は更新先と同じ実体。
Vector3& operator+=(Vector3& v1, const Vector3& v2) {
	v1.x += v2.x;
	v1.y += v2.y;
	v1.z += v2.z;
	return v1;
}

Matrix4x4 Math::MakeIdentity4x4() {
    // 行ベクトルを使う変換群の基準として、対角だけを1にします。

	Matrix4x4 result = {};
	result.m[0][0] = 1.0f;
	result.m[0][1] = 0.0f;
	result.m[0][2] = 0.0f;
	result.m[0][3] = 0.0f;

	result.m[1][0] = 0.0f;
	result.m[1][1] = 1.0f;
	result.m[1][2] = 0.0f;
	result.m[1][3] = 0.0f;

	result.m[2][0] = 0.0f;
	result.m[2][1] = 0.0f;
	result.m[2][2] = 1.0f;
	result.m[2][3] = 0.0f;

	result.m[3][0] = 0.0f;
	result.m[3][1] = 0.0f;
	result.m[3][2] = 0.0f;
	result.m[3][3] = 1.0f;
	return result;
}

Matrix4x4 Math::MakeScaleMatrix(const Vector3& scale) {
    // 各軸の倍率を対角成分に置き、平行移動は加えません。
	Matrix4x4 result{ scale.x, 0.0f, 0.0f, 0.0f, 0.0f, scale.y, 0.0f, 0.0f, 0.0f, 0.0f, scale.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

	return result;
}

Matrix4x4 Math::MakeRotateXMatrix(float theta) {
    // thetaはラジアンで受け取り、X軸回りの行ベクトル用行列を作ります。
	float sin = std::sin(theta);
	float cos = std::cos(theta);

	Matrix4x4 result{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cos, sin, 0.0f, 0.0f, -sin, cos, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

	return result;
}

Matrix4x4 Math::MakeRotateYMatrix(float theta) {
    // thetaはラジアンで受け取り、Y軸回りの行ベクトル用行列を作ります。
	float sin = std::sin(theta);
	float cos = std::cos(theta);

	Matrix4x4 result{ cos, 0.0f, -sin, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, sin, 0.0f, cos, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

	return result;
}

Matrix4x4 Math::MakeRotateZMatrix(float theta) {
    // thetaはラジアンで受け取り、Z軸回りの行ベクトル用行列を作ります。
	float sin = std::sin(theta);
	float cos = std::cos(theta);

	Matrix4x4 result{ cos, sin, 0.0f, 0.0f, -sin, cos, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };

	return result;
}

Matrix4x4 Math::MakeTranslateMatrix(const Vector3& translate) {
    // 行ベクトル規約に合わせ、平行移動を最終行に置きます。
	Matrix4x4 result{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, translate.x, translate.y, translate.z, 1.0f };

	return result;
}

Matrix4x4 Math::Multiply(const Matrix4x4& m1, const Matrix4x4& m2) {
    // 左から右への変換順を保つため、行と列の内積で積を作ります。
	Matrix4x4 result = {};

	for (std::int32_t row = 0; row < 4; ++row) {
		for (std::int32_t col = 0; col < 4; ++col) {
			result.m[row][col] =
				m1.m[row][0] * m2.m[0][col] +
				m1.m[row][1] * m2.m[1][col] +
				m1.m[row][2] * m2.m[2][col] +
				m1.m[row][3] * m2.m[3][col];
		}
	}

	return result;
}

// 余因子から逆行列を作る。変換を逆にたどれないときはゼロ行列を返す。
Matrix4x4 Math::Inverse(const Matrix4x4& m) {
	Matrix4x4 result = {};


	float a11 = m.m[0][0], a12 = m.m[0][1], a13 = m.m[0][2], a14 = m.m[0][3];
	float a21 = m.m[1][0], a22 = m.m[1][1], a23 = m.m[1][2], a24 = m.m[1][3];
	float a31 = m.m[2][0], a32 = m.m[2][1], a33 = m.m[2][2], a34 = m.m[2][3];
	float a41 = m.m[3][0], a42 = m.m[3][1], a43 = m.m[3][2], a44 = m.m[3][3];


	// 4×4行列の行列式です。ゼロなら変換で情報が失われ、一意に元の点へ戻せません。
	float det = a11 * a22 * a33 * a44 + a11 * a23 * a34 * a42 + a11 * a24 * a32 * a43
		- a11 * a24 * a33 * a42 - a11 * a23 * a32 * a44 - a11 * a22 * a34 * a43
		- a12 * a21 * a33 * a44 - a13 * a21 * a34 * a42 - a14 * a21 * a32 * a43
		+ a14 * a21 * a33 * a42 + a13 * a21 * a32 * a44 + a12 * a21 * a34 * a43
		+ a12 * a23 * a31 * a44 + a13 * a24 * a31 * a42 + a14 * a22 * a31 * a43
		- a14 * a23 * a31 * a42 - a13 * a22 * a31 * a44 - a12 * a24 * a31 * a43
		- a12 * a23 * a34 * a41 - a13 * a24 * a32 * a41 - a14 * a22 * a33 * a41
		+ a14 * a23 * a32 * a41 + a13 * a22 * a34 * a41 + a12 * a24 * a33 * a41;
	// 行列式が厳密に0なら、逆変換を作らずゼロ行列を返す。
	if (det == 0.0f) {
		return result;
	}

	// 余因子を転置して作る随伴行列の全要素に、同じ行列式の逆数を掛けます。
	float invDet = 1.0f / det;


	// 1行目
	result.m[0][0] = (a22 * (a33 * a44 - a34 * a43) - a23 * (a32 * a44 - a34 * a42) + a24 * (a32 * a43 - a33 * a42)) * invDet;
	result.m[0][1] = -(a12 * (a33 * a44 - a34 * a43) - a13 * (a32 * a44 - a34 * a42) + a14 * (a32 * a43 - a33 * a42)) * invDet;
	result.m[0][2] = (a12 * (a23 * a44 - a24 * a43) - a13 * (a22 * a44 - a24 * a42) + a14 * (a22 * a43 - a23 * a42)) * invDet;
	result.m[0][3] = -(a12 * (a23 * a34 - a24 * a33) - a13 * (a22 * a34 - a24 * a32) + a14 * (a22 * a33 - a23 * a32)) * invDet;

	// 2行目
	result.m[1][0] = -(a21 * (a33 * a44 - a34 * a43) - a23 * (a31 * a44 - a34 * a41) + a24 * (a31 * a43 - a33 * a41)) * invDet;
	result.m[1][1] = (a11 * (a33 * a44 - a34 * a43) - a13 * (a31 * a44 - a34 * a41) + a14 * (a31 * a43 - a33 * a41)) * invDet;
	result.m[1][2] = -(a11 * (a23 * a44 - a24 * a43) - a13 * (a21 * a44 - a24 * a41) + a14 * (a21 * a43 - a23 * a41)) * invDet;
	result.m[1][3] = (a11 * (a23 * a34 - a24 * a33) - a13 * (a21 * a34 - a24 * a31) + a14 * (a21 * a33 - a23 * a31)) * invDet;

	// 3行目
	result.m[2][0] = (a21 * (a32 * a44 - a34 * a42) - a22 * (a31 * a44 - a34 * a41) + a24 * (a31 * a42 - a32 * a41)) * invDet;
	result.m[2][1] = -(a11 * (a32 * a44 - a34 * a42) - a12 * (a31 * a44 - a34 * a41) + a14 * (a31 * a42 - a32 * a41)) * invDet;
	result.m[2][2] = (a11 * (a22 * a44 - a24 * a42) - a12 * (a21 * a44 - a24 * a41) + a14 * (a21 * a42 - a22 * a41)) * invDet;
	result.m[2][3] = -(a11 * (a22 * a34 - a24 * a32) - a12 * (a21 * a34 - a24 * a31) + a14 * (a21 * a32 - a22 * a31)) * invDet;

	// 4行目
	result.m[3][0] = -(a21 * (a32 * a43 - a33 * a42) - a22 * (a31 * a43 - a33 * a41) + a23 * (a31 * a42 - a32 * a41)) * invDet;
	result.m[3][1] = (a11 * (a32 * a43 - a33 * a42) - a12 * (a31 * a43 - a33 * a41) + a13 * (a31 * a42 - a32 * a41)) * invDet;
	result.m[3][2] = -(a11 * (a22 * a43 - a23 * a42) - a12 * (a21 * a43 - a23 * a41) + a13 * (a21 * a42 - a22 * a41)) * invDet;
	result.m[3][3] = (a11 * (a22 * a33 - a23 * a32) - a12 * (a21 * a33 - a23 * a31) + a13 * (a21 * a32 - a22 * a31)) * invDet;

	return result;
}

Matrix4x4 Math::MakePerspectiveFovMatrix(float fovY, float aspectRatio, float nearClip, float farClip) {
    // fovYはラジアンとし、DirectXの深度0～1へ写す透視投影を作ります。
	Matrix4x4 result = {};
	// 縦方向は半画角の正接の逆数で拡大します。横方向は画面の横幅と縦幅の比で割って補正します。
	float f = 1.0f / tanf(fovY / 2.0f);
	result.m[0][0] = f / aspectRatio;
	result.m[0][1] = 0.0f;
	result.m[0][2] = 0.0f;
	result.m[0][3] = 0.0f;

	result.m[1][0] = 0.0f;
	result.m[1][1] = f;
	result.m[1][2] = 0.0f;
	result.m[1][3] = 0.0f;

	result.m[2][0] = 0.0f;
	result.m[2][1] = 0.0f;
	// 同次座標の w に視線方向の距離を入れ、透視除算後の深度が近面で0、遠面で1となるよう係数をそろえます。
	result.m[2][2] = farClip / (farClip - nearClip);
	result.m[2][3] = 1.0f;

	result.m[3][0] = 0.0f;
	result.m[3][1] = 0.0f;
	result.m[3][2] = -nearClip * farClip / (farClip - nearClip);
	result.m[3][3] = 0.0f;
	return result;
}

Matrix4x4 Math::MakeOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip) {
    // 指定矩形をNDCへ写し、深度だけ0～1へ正規化します。
	Matrix4x4 result{};
	result.m[0][0] = 2.0f / (right - left);
	result.m[1][1] = 2.0f / (top - bottom);
	result.m[2][2] = 1.0f / (farClip - nearClip);
	result.m[3][0] = (left + right) / (left - right);
	result.m[3][1] = (top + bottom) / (bottom - top);
	result.m[3][2] = nearClip / (nearClip - farClip);
	result.m[3][3] = 1.0f;
	return result;
}

// NDC を画面ピクセル座標と指定した深度範囲へ写します。
Matrix4x4 Math::MakeViewportMatrix(float left, float top, float width, float height, float minDepth, float maxDepth) {
	Matrix4x4 result = {};
	result.m[0][0] = width / 2.0f;
	result.m[0][1] = 0.0f;
	result.m[0][2] = 0.0f;
	result.m[0][3] = 0.0f;

	result.m[1][0] = 0.0f;
	result.m[1][1] = -height / 2.0f; // Y軸反転（スクリーン座標系へ）
	result.m[1][2] = 0.0f;
	result.m[1][3] = 0.0f;

	result.m[2][0] = 0.0f;
	result.m[2][1] = 0.0f;
	result.m[2][2] = maxDepth - minDepth;
	result.m[2][3] = 0.0f;

	// NDCの中心を、表示領域の左上に半幅と半高を足した画面上の中心へ移します。
	result.m[3][0] = left + width / 2.0f;
	result.m[3][1] = top + height / 2.0f;
	result.m[3][2] = minDepth;
	result.m[3][3] = 1.0f;

	return result;
}

Matrix4x4 Math::MakeAffineMatrix(const Vector3& scale, const Vector3& rotate, const Vector3& translate) {
    // Euler角はラジアンで、拡大→X/Y/Z回転→平行移動の順に合成します。
	Matrix4x4 scaleMatrix = MakeScaleMatrix(scale);
	Matrix4x4 rotateXMatrix = MakeRotateXMatrix(rotate.x);
	Matrix4x4 rotateYMatrix = MakeRotateYMatrix(rotate.y);
	Matrix4x4 rotateZMatrix = MakeRotateZMatrix(rotate.z);
	Matrix4x4 rotateMatrix = Multiply(Multiply(rotateXMatrix, rotateYMatrix), rotateZMatrix);
	Matrix4x4 translateMatrix = MakeTranslateMatrix(translate);

	// 点を行ベクトルとして左に置くため、左側の拡大と回転から適用されます。最後の移動量は拡大されません。
	Matrix4x4 worldMatrix = Multiply(Multiply(scaleMatrix, rotateMatrix), translateMatrix);

	return worldMatrix;
}

Vector3 Math::Normalize(const Vector3& v) {
	float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);

	// ほぼ長さ0の方向は除算で丸め誤差が増幅するため、単位化せずゼロ方向を返します。
	if (length < 0.000001f) {
		return { 0.0f, 0.0f, 0.0f };
	}

	return { v.x / length, v.y / length, v.z / length };
}

// ラジアンの軸回転をZ→X→Yの順に合成する。
Matrix4x4 Math::MakeRotateMatrix(const Vector3& rotate) {
	Matrix4x4 rotX = MakeRotateXMatrix(rotate.x);
	Matrix4x4 rotY = MakeRotateYMatrix(rotate.y);
	Matrix4x4 rotZ = MakeRotateZMatrix(rotate.z);
	return Multiply(Multiply(rotZ, rotX), rotY); // Z→X→Y の順
}

Vector3 Math::TransformNormal(const Vector3& v, const Matrix4x4& m) {
    // 方向ベクトルには平行移動成分を適用しません。
	// 実質的に同次座標のw=0を使い、行列の左上3×3だけで方向を変換します。
	Vector3 result;
	result.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0];
	result.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1];
	result.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2];
	return result;
}

float Math::Dot(const Vector3& v1, const Vector3& v2) {
    // 内積は角度比較や長さの二乗にも共通利用します。
	return v1.x * v2.x + v1.y * v2.y + v1.z * v2.z;
}

float Math::Length(const Vector3& v) {
    // 内積から長さを求め、各成分を別々に平方し直しません。
	return std::sqrt(Dot(v, v));
}

Vector3 Math::Cross(const Vector3& v1, const Vector3& v2) {
    // 右手系の外積順を保ち、向きの反転を避けます。
	return {
		v1.y * v2.z - v1.z * v2.y,
		v1.z * v2.x - v1.x * v2.z,
		v1.x * v2.y - v1.y * v2.x
	};
}

Matrix4x4 Math::MakeLookAtMatrix(const Vector3& eye, const Vector3& target, const Vector3& up) {
    // 視線と上方向から直交基底を作り、カメラ行列へ配置します。
	Vector3 zaxis = Normalize(target - eye);
	Vector3 xaxis = Normalize(Cross(up, zaxis));
	Vector3 yaxis = Cross(zaxis, xaxis);

	Matrix4x4 result = MakeIdentity4x4();
	result.m[0][0] = xaxis.x;
	result.m[0][1] = yaxis.x;
	result.m[0][2] = zaxis.x;
	result.m[1][0] = xaxis.y;
	result.m[1][1] = yaxis.y;
	result.m[1][2] = zaxis.y;
	result.m[2][0] = xaxis.z;
	result.m[2][1] = yaxis.z;
	result.m[2][2] = zaxis.z;
	// 視点をカメラ各軸へ射影して引き、カメラ位置をビュー座標の原点へ移します。
	result.m[3][0] = -Dot(eye, xaxis);
	result.m[3][1] = -Dot(eye, yaxis);
	result.m[3][2] = -Dot(eye, zaxis);

	return result;
}

float Math::Clamp(float value, float min, float max) {
    // 範囲外の値だけ境界へ寄せ、範囲内は変更しません。
	// 下限より小さい値は下限へ揃える。
	if (value < min) {
		return min;
	}

	// 上限より大きい値は上限へ揃える。
	if (value > max) {
		return max;
	}

	return value;
}

float Math::Lerp(float v1, float v2, float t) {
	// t=0でv1、t=1でv2。範囲外を渡すと同じ直線上へ外挿します。
	return v1 + (v2 - v1) * t;
}

Vector4 Math::Lerp(const Vector4& v1, const Vector4& v2, float t) {
	// Vector4 の各要素 (x, y, z, w) に対して Lerp を行う
	Vector4 result;
	result.x = Lerp(v1.x, v2.x, t);
	result.y = Lerp(v1.y, v2.y, t);
	result.z = Lerp(v1.z, v2.z, t);
	result.w = Lerp(v1.w, v2.w, t);
	return result;
}

// 回転の直交基底などで使うため、行と列を入れ替えた行列を返す。
Matrix4x4 Math::Transpose(const Matrix4x4& m) {
	Matrix4x4 result;

	for (std::int32_t i = 0; i < 4; ++i) {
		for (std::int32_t j = 0; j < 4; ++j) {
			result.m[i][j] = m.m[j][i]; // 行と列を入れ替える
		}
	}

	return result;
}

// 位置を同次座標として変換し、必要なら透視除算する。
Vector3 Math::Transform(const Vector3& v, const Matrix4x4& m) {
	Vector3 result;

	// 位置には平行移動を加え、同時に除算用のwも求める。
	result.x = v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0];
	result.y = v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1];
	result.z = v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2];
	float w = v.x * m.m[0][3] + v.y * m.m[1][3] + v.z * m.m[2][3] + m.m[3][3];

	// wが0なら透視除算できないため、割る前の座標を返します。
	if (w != 0.0f) {
		result.x /= w;
		result.y /= w;
		result.z /= w;
	}

	return result;
}


// X・Y・Zの各範囲を通過できるレイ上の区間を重ね、最も近い有効な交点を求める。
bool Math::IntersectRayAabb(const Ray& ray, const Vector3& minBox, const Vector3& maxBox, RayResult* hit) {
	float tMin = 0.0f;
	// 交差位置は origin + diff × t で表します。diff が単位長なら t は距離になります。
	float tMax = 100000.0f; // レイのパラメーターtの上限。

	// X成分がほぼ0なら、レイに沿ってXの範囲をまたがない。
	if (std::abs(ray.diff.x) < 1e-6f) {
		// X成分が変わらず始点が箱のX範囲外なら、交差する区間が存在しない。
		if (ray.origin.x < minBox.x || ray.origin.x > maxBox.x)
			return false;
	}
	// X成分が有効なら、Xの二つの境界に達する時刻を求める。
	else {
		float invD = 1.0f / ray.diff.x;
		float t1 = (minBox.x - ray.origin.x) * invD;
		float t2 = (maxBox.x - ray.origin.x) * invD;
		// 進行がXの負向きなら最小面の時刻が後になる。先に入る時刻をt1へ置く。
		if (t1 > t2)
			std::swap(t1, t2);
		tMin = std::max(tMin, t1);
		tMax = std::min(tMax, t2);
		// 箱のX範囲を通る区間と探索区間に共通部分がなければ交差しない。
		if (tMin > tMax)
			return false;
	}

	// Y方向の移動成分がほぼ0の場合は除算せず、始点がYの範囲内かだけ調べます。
	if (std::abs(ray.diff.y) < 1e-6f) {
		// 始点のYが箱の外側で、レイのY成分がほぼ0なら箱へ入れない。
		if (ray.origin.y < minBox.y || ray.origin.y > maxBox.y)
			return false;
	}
	// Y成分が有効なら、Y方向で箱に入る時刻と出る時刻を求める。
	else {
		float invD = 1.0f / ray.diff.y;
		float t1 = (minBox.y - ray.origin.y) * invD;
		float t2 = (maxBox.y - ray.origin.y) * invD;
		// Yの負向きでは最小面の時刻が後になるため、入る時刻を先に揃える。
		if (t1 > t2)
			std::swap(t1, t2);
		tMin = std::max(tMin, t1);
		tMax = std::min(tMax, t2);
		// Xで絞った区間とYの通過区間が重ならなければ、箱には当たらない。
		if (tMin > tMax)
			return false;
	}

	// Z成分がほぼ0の場合も、始点がZの範囲外なら他軸で交差していても棄却します。
	if (std::abs(ray.diff.z) < 1e-6f) {
		// 始点のZが箱の外側で、レイのZ成分がほぼ0なら箱へ入れない。
		if (ray.origin.z < minBox.z || ray.origin.z > maxBox.z)
			return false;
	}
	// Z成分が有効なら、Z方向の通過区間を求めて他の軸と重ねる。
	else {
		float invD = 1.0f / ray.diff.z;
		float t1 = (minBox.z - ray.origin.z) * invD;
		float t2 = (maxBox.z - ray.origin.z) * invD;
		// Zの負向きでは最小面の時刻が後になるため、入る時刻と出る時刻を入れ替える。
		if (t1 > t2)
			std::swap(t1, t2);
		tMin = std::max(tMin, t1);
		tMax = std::min(tMax, t2);
		// 三軸すべてを通過できる時刻区間が残らなければ交差しない。
		if (tMin > tMax)
			return false;
	}

	// 出力先が指定された場合だけ、共通区間の最初の点を記録する。
	if (hit) {
		hit->isHit = true;
		hit->distance = tMin;

		// 衝突点 = origin + diff * tMin
		hit->point.x = ray.origin.x + ray.diff.x * tMin;
		hit->point.y = ray.origin.y + ray.diff.y * tMin;
		hit->point.z = ray.origin.z + ray.diff.z * tMin;

		// 箱の中心を原点にして各面の近さを調べ、当たった面の向きの目安を作る。
		Vector3 center = { (minBox.x + maxBox.x) * 0.5f, (minBox.y + maxBox.y) * 0.5f, (minBox.z + maxBox.z) * 0.5f };
		Vector3 size = { (maxBox.x - minBox.x) * 0.5f, (maxBox.y - minBox.y) * 0.5f, (maxBox.z - minBox.z) * 0.5f };
		Vector3 p = { hit->point.x - center.x, hit->point.y - center.y, hit->point.z - center.z };

		// 検出点の丸め誤差を吸収するため、面と見なす境界を箱の内側へ少し寄せます。
		float bias = 1.001f; // 誤差対策
		hit->normal = { 0, 0, 0 };

		// 中心から見て+Xの面に十分近ければ、+Xを法線とする。
		if (p.x >= size.x / bias)
			hit->normal = { 1, 0, 0 };
		// -X側の面に十分近ければ、-Xを法線とする。
		else if (p.x <= -size.x / bias) hit->normal = { -1, 0, 0 };
		// X面に該当せず+Yの側面条件を満たせば、+Yを法線とする。
		else if (p.y >= size.y / bias) hit->normal = { 0, 1, 0 };
		// Yの下端に十分近ければ、-Yを法線とする。
		else if (p.y <= -size.y / bias) hit->normal = { 0, -1, 0 };
		// XとYの面に該当せず+Zの端なら、+Zを法線とする。
		else if (p.z >= size.z / bias) hit->normal = { 0, 0, 1 };
		// Zの負側の端なら、-Zを法線とする。どの面にも近くなければ初期値のゼロを残す。
		else if (p.z <= -size.z / bias) hit->normal = { 0, 0, -1 };
	}

	return true;
}

// ラジアンの角度同士を、周期を考慮して短い向きに補間する。
float Math::LerpShortAngle(float a, float b, float t) {
	float diff = b - a;

	constexpr float kTwoPi = std::numbers::pi_v<float> * 2.0f;

	// 差を±πラジアンへ畳み、どちら回りが短いかを符号に残します。
	while (diff < -std::numbers::pi_v<float>)
		diff += kTwoPi;
	while (diff > std::numbers::pi_v<float>)
		diff -= kTwoPi;

	return a + diff * t;
}

Vector3 Math::Lerp(const Vector3& v1, const Vector3& v2, float t) {
    // tはクランプせず、必要なら外挿も呼び出し側で利用できます。
	return {
		v1.x + (v2.x - v1.x) * t,
		v1.y + (v2.y - v1.y) * t,
		v1.z + (v2.z - v1.z) * t
	};
}

// 正規化済みの二つの回転を補間する。符号と近さから球面補間の重みを決める。
Quaternion Math::Slerp(const Quaternion& q0, const Quaternion& q1, float t) {
	float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;

	// q1と-q1は同じ姿勢を表すため、必要なら内積を正側へ戻せる。
	Quaternion trueQ1 = q1;

	// 内積が負の場合に片側の符号を反転し、姿勢を保ちながら4次元上の短い弧を選ぶ。
	if (dot < 0.0f) {
		trueQ1 = { -q1.x, -q1.y, -q1.z, -q1.w };
		dot = -dot;
	}

	// 内積が1に近ければsinがほぼ0のため、成分の線形補間を使う。
	if (dot >= 1.0f - 0.0005f) {
		return {
			q0.x + (trueQ1.x - q0.x) * t,
			q0.y + (trueQ1.y - q0.y) * t,
			q0.z + (trueQ1.z - q0.z) * t,
			q0.w + (trueQ1.w - q0.w) * t
		};
	}

	// 単位クォータニオン間の角度をラジアンで求め、弧の両側の長さに応じて正弦で重みを付けます。
	float theta = std::acos(dot);
	float sinTheta = std::sin(theta);

	float scale0 = std::sin((1.0f - t) * theta) / sinTheta;
	float scale1 = std::sin(t * theta) / sinTheta;

	return {
		q0.x * scale0 + trueQ1.x * scale1,
		q0.y * scale0 + trueQ1.y * scale1,
		q0.z * scale0 + trueQ1.z * scale1,
		q0.w * scale0 + trueQ1.w * scale1
	};
}

Matrix4x4 Math::MakeRotateQuaternionMatrix(const Quaternion& q) {
    // 正規化済みQuaternionを前提に、回転だけを4x4へ展開します。
	Matrix4x4 m;
	float x = q.x, y = q.y, z = q.z, w = q.w;

	m.m[0][0] = 1 - 2 * y * y - 2 * z * z;
	m.m[0][1] = 2 * x * y + 2 * w * z;
	m.m[0][2] = 2 * x * z - 2 * w * y;
	m.m[0][3] = 0;

	m.m[1][0] = 2 * x * y - 2 * w * z;
	m.m[1][1] = 1 - 2 * x * x - 2 * z * z;
	m.m[1][2] = 2 * y * z + 2 * w * x;
	m.m[1][3] = 0;

	m.m[2][0] = 2 * x * z + 2 * w * y;
	m.m[2][1] = 2 * y * z - 2 * w * x;
	m.m[2][2] = 1 - 2 * x * x - 2 * y * y;
	m.m[2][3] = 0;

	m.m[3][0] = 0;
	m.m[3][1] = 0;
	m.m[3][2] = 0;
	m.m[3][3] = 1;

	return m;
}

// 原点を画面中央とした正射影です。幅・高さの両端を±1、近面・遠面を深度0と1へ写します。
Matrix4x4 Math::MakeOrthographicMatrix(float width, float height, float nearZ, float farZ) {
	Matrix4x4 result{}; // ゼロ初期化

	result.m[0][0] = 2.0f / width;
	result.m[1][1] = 2.0f / height;
	result.m[2][2] = 1.0f / (farZ - nearZ);
	result.m[3][2] = -nearZ / (farZ - nearZ);
	result.m[3][3] = 1.0f;

	return result;
}

// ラジアンのEuler角を軸ごとの四元数にし、Z→X→Yの順で作用するよう合成する。
Quaternion Math::EulerToQuaternion(const Vector3& rot) {
	// 軸回転のクォータニオンは虚部に軸方向×sin(角度/2)、実部にcos(角度/2)を持ちます。角度はラジアンです。
	float cx = std::cos(rot.x * 0.5f);
	float sx = std::sin(rot.x * 0.5f);
	float cy = std::cos(rot.y * 0.5f);
	float sy = std::sin(rot.y * 0.5f);
	float cz = std::cos(rot.z * 0.5f);
	float sz = std::sin(rot.z * 0.5f);

	Quaternion qX = { sx, 0.0f, 0.0f, cx };
	Quaternion qY = { 0.0f, sy, 0.0f, cy };
	Quaternion qZ = { 0.0f, 0.0f, sz, cz };

	// 四元数の積は右から作用するので、使う順と逆に並べて合成する。
	return (qY * qX) * qZ;
}

// 軸スケールを取り除ける場合に取り除き、Z→X→Yの角度を抽出する。
Vector3 Math::MatrixToEuler(const Matrix4x4& m) {
	Vector3 euler;

	// 行ベクトル用の行列では、左上3×3の各行の長さを軸ごとの倍率として扱う。
	Vector3 scale;
	scale.x = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
	scale.y = std::sqrt(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
	scale.z = std::sqrt(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);

	Matrix4x4 rmat = m;

	// Xに対応する行の長さが十分あれば、その行から倍率を取り除く。
	if (scale.x > 0.0001f) {
		rmat.m[0][0] /= scale.x;
		rmat.m[0][1] /= scale.x;
		rmat.m[0][2] /= scale.x;
	}
	// Yに対応する行の長さが十分あれば、その行から倍率を取り除く。
	if (scale.y > 0.0001f) {
		rmat.m[1][0] /= scale.y;
		rmat.m[1][1] /= scale.y;
		rmat.m[1][2] /= scale.y;
	}
	// Zに対応する行の長さが十分あれば、その行から倍率を取り除く。
	if (scale.z > 0.0001f) {
		rmat.m[2][0] /= scale.z;
		rmat.m[2][1] /= scale.z;
		rmat.m[2][2] /= scale.z;
	}

	// Xの傾きを先に求め、その角度からYとZを独立に取り出せるか判定する。
	float sinX = -rmat.m[1][2];

	// 丸め誤差でasinの定義域を外れないよう、sinの入力を-1から1に制限する。
	sinX = std::max(-1.0f, std::min(1.0f, sinX));
	euler.x = std::asin(sinX);

	// Xが±90度に十分近くない場合は、YとZを個別に求められる。
	if (std::abs(rmat.m[1][2]) < 0.9999f) {
		euler.y = std::atan2(rmat.m[0][2], rmat.m[2][2]);
		euler.z = std::atan2(rmat.m[1][0], rmat.m[1][1]);
	}
	// Xが±90度に近い場合は、YとZを独立に決められないためZを0とする。
	else {
		// ジンバルロック時は Zを0に固定し、Yだけで表現する
		euler.y = std::atan2(-rmat.m[2][0], rmat.m[0][0]);
		euler.z = 0.0f;
	}

	return euler;
}

// 軸スケールを除いた行列から四元数を取り出し、長さを1に揃える。
Quaternion Math::MatrixToQuaternion(const Matrix4x4& m) {
	// 各行の長さを軸スケールとして見積もり、倍率を除く。
	Vector3 scale;
	scale.x = std::sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]);
	scale.y = std::sqrt(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]);
	scale.z = std::sqrt(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2]);

	Matrix4x4 rmat = m;

	// Xに対応する行の長さが十分あるときだけ除算する。
	if (scale.x > 0.0001f) {
		rmat.m[0][0] /= scale.x;
		rmat.m[0][1] /= scale.x;
		rmat.m[0][2] /= scale.x;
	}
	// Yに対応する行の長さが十分あるときだけ除算する。
	if (scale.y > 0.0001f) {
		rmat.m[1][0] /= scale.y;
		rmat.m[1][1] /= scale.y;
		rmat.m[1][2] /= scale.y;
	}
	// Zに対応する行の長さが十分あるときだけ除算する。
	if (scale.z > 0.0001f) {
		rmat.m[2][0] /= scale.z;
		rmat.m[2][1] /= scale.z;
		rmat.m[2][2] /= scale.z;
	}

	// 対角成分から、分母に使う値を十分大きく取れる計算式を選ぶ。
	Quaternion q;
	// 対角和が正なら w を先に、そのほかは最大の対角成分に対応する軸を先に求めます。小さな成分を分母にして誤差を広げないためです。
	float tr = rmat.m[0][0] + rmat.m[1][1] + rmat.m[2][2];

	// 対角和が正なら、まず実部wを求めて残りの成分を計算する。
	if (tr > 0.0f) {
		float s = std::sqrt(tr + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (rmat.m[1][2] - rmat.m[2][1]) / s;
		q.y = (rmat.m[2][0] - rmat.m[0][2]) / s;
		q.z = (rmat.m[0][1] - rmat.m[1][0]) / s;
	}
	// 対角和が正でなくXの対角項が最大なら、xを先に求める。
	else if ((rmat.m[0][0] >= rmat.m[1][1]) && (rmat.m[0][0] >= rmat.m[2][2])) {
		float s = std::sqrt(1.0f + rmat.m[0][0] - rmat.m[1][1] - rmat.m[2][2]) * 2.0f;
		q.w = (rmat.m[1][2] - rmat.m[2][1]) / s;
		q.x = 0.25f * s;
		q.y = (rmat.m[0][1] + rmat.m[1][0]) / s;
		q.z = (rmat.m[2][0] + rmat.m[0][2]) / s;
	}
	// Xを使わなかった場合にYの対角項がZ以上なら、yを先に求める。
	else if (rmat.m[1][1] >= rmat.m[2][2]) {
		float s = std::sqrt(1.0f + rmat.m[1][1] - rmat.m[0][0] - rmat.m[2][2]) * 2.0f;
		q.w = (rmat.m[2][0] - rmat.m[0][2]) / s;
		q.x = (rmat.m[0][1] + rmat.m[1][0]) / s;
		q.y = 0.25f * s;
		q.z = (rmat.m[1][2] + rmat.m[2][1]) / s;
	}
	// 残る場合はZの対角項が最大なので、zを先に求める。
	else {
		float s = std::sqrt(1.0f + rmat.m[2][2] - rmat.m[0][0] - rmat.m[1][1]) * 2.0f;
		q.w = (rmat.m[0][1] - rmat.m[1][0]) / s;
		q.x = (rmat.m[2][0] + rmat.m[0][2]) / s;
		q.y = (rmat.m[1][2] + rmat.m[2][1]) / s;
		q.z = 0.25f * s;
	}

	// 行列からの変換で生じた丸め誤差を除き、ゼロ長なら単位クォータニオンへ戻します。
	float vectorLength = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);

	// 求めた四元数の長さが十分あれば、全成分をその長さで割る。
	if (vectorLength > 0.0001f) {
		q.x /= vectorLength;
		q.y /= vectorLength;
		q.z /= vectorLength;
		q.w /= vectorLength;
	}
	// ほぼゼロ長の場合は、単位回転を表す四元数に置き換える。
	else {
		q = { 0.0f, 0.0f, 0.0f, 1.0f }; // 安全な初期値
	}

	return q;
}


Frustum Math::ExtractFrustumPlanes(const Matrix4x4& vp) {
	// クリップ座標の左右・上下は −w～w、深度は 0～w です。各不等式が成り立つ範囲をワールド空間の面に戻します。
	Frustum f;

	// 左
	f.planes[0].normal.x = vp.m[0][3] + vp.m[0][0];
	f.planes[0].normal.y = vp.m[1][3] + vp.m[1][0];
	f.planes[0].normal.z = vp.m[2][3] + vp.m[2][0];
	f.planes[0].distance = vp.m[3][3] + vp.m[3][0];

	// 右
	f.planes[1].normal.x = vp.m[0][3] - vp.m[0][0];
	f.planes[1].normal.y = vp.m[1][3] - vp.m[1][0];
	f.planes[1].normal.z = vp.m[2][3] - vp.m[2][0];
	f.planes[1].distance = vp.m[3][3] - vp.m[3][0];

	// 下
	f.planes[2].normal.x = vp.m[0][3] + vp.m[0][1];
	f.planes[2].normal.y = vp.m[1][3] + vp.m[1][1];
	f.planes[2].normal.z = vp.m[2][3] + vp.m[2][1];
	f.planes[2].distance = vp.m[3][3] + vp.m[3][1];

	// 上
	f.planes[3].normal.x = vp.m[0][3] - vp.m[0][1];
	f.planes[3].normal.y = vp.m[1][3] - vp.m[1][1];
	f.planes[3].normal.z = vp.m[2][3] - vp.m[2][1];
	f.planes[3].distance = vp.m[3][3] - vp.m[3][1];

	// 近 (DirectX12は0〜w)
	f.planes[4].normal.x = vp.m[0][2];
	f.planes[4].normal.y = vp.m[1][2];
	f.planes[4].normal.z = vp.m[2][2];
	f.planes[4].distance = vp.m[3][2];

	// 遠
	f.planes[5].normal.x = vp.m[0][3] - vp.m[0][2];
	f.planes[5].normal.y = vp.m[1][3] - vp.m[1][2];
	f.planes[5].normal.z = vp.m[2][3] - vp.m[2][2];
	f.planes[5].distance = vp.m[3][3] - vp.m[3][2];

	// 法線を単位長にするときは、同じ割合で平面の定数項も変換する。
	for (std::int32_t i = 0; i < 6; ++i) {
		float length = std::sqrt(f.planes[i].normal.x * f.planes[i].normal.x +
			f.planes[i].normal.y * f.planes[i].normal.y +
			f.planes[i].normal.z * f.planes[i].normal.z);
		// 法線の長さが正の面だけを正規化し、ゼロで割らない。
		if (length > 0.0f) {
			f.planes[i].normal = f.planes[i].normal / length;
			f.planes[i].distance /= length;
		}
	}

	return f;
}

// 視錐台のどの一面から見ても、箱全体が外側へ出ていないか調べる。
bool Math::IntersectFrustumAabb(const Frustum& f, const Vector3& minBox, const Vector3& maxBox) {
	for (std::int32_t i = 0; i < 6; ++i) {
		// 法線との内積が最大になる箱の頂点を求める。全軸が負の場合は最小点がその頂点になる。
		Vector3 p = minBox;
		// 法線が+X側を向いていれば、頂点には箱の最大Xを選ぶ。
		if (f.planes[i].normal.x >= 0)
			p.x = maxBox.x;
		// 法線が+Y側を向いていれば、頂点には箱の最大Yを選ぶ。
		if (f.planes[i].normal.y >= 0)
			p.y = maxBox.y;
		// 法線が+Z側を向いていれば、頂点には箱の最大Zを選ぶ。
		if (f.planes[i].normal.z >= 0)
			p.z = maxBox.z;

		// この点が面の外にあれば、他の頂点もすべてその外側にある。
		float dot = f.planes[i].normal.x * p.x + f.planes[i].normal.y * p.y + f.planes[i].normal.z * p.z;

		// 平面式が負なら、箱全体がその面の外側にあるため描画範囲から外す。
		if (dot + f.planes[i].distance < 0) {
			return false;
		}
	}

	return true;
}

float Math::DistanceSquaredPointAabb(const Vector3& point, const Vector3& minBox, const Vector3& maxBox) {
    // 箱の範囲内にある軸は距離0とし、残りの箱外距離を二乗して足します。
    const auto distanceOutsideAxis = [](float value, float min, float max) {
        if (value < min) {
            return min - value;
        }
        if (value > max) {
            return value - max;
        }
        return 0.0f;
    };
    const float dx = distanceOutsideAxis(point.x, minBox.x, maxBox.x);
    const float dy = distanceOutsideAxis(point.y, minBox.y, maxBox.y);
    const float dz = distanceOutsideAxis(point.z, minBox.z, maxBox.z);
	return dx * dx + dy * dy + dz * dz;
}

} // namespace engine

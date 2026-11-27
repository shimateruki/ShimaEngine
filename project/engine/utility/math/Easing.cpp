#include "Easing.h"


float Easing::Linear(float t) {
    // 時間率を変化させず、一定速度の補間に使う。
    return t;
}

float Easing::InSine(float t) {
    // 開始時の速度をゼロへ近づけ、徐々に加速する。
    return 1.0f - std::cos((t * std::numbers::pi_v<float>) / 2.0f);
}

float Easing::OutSine(float t) {
    // 終了時の速度をゼロへ近づけ、着地や停止を滑らかにする。
    return std::sin((t * std::numbers::pi_v<float>) / 2.0f);
}

float Easing::InOutSine(float t) {
    // 開始と終了の両端を減速し、中間で最も速く動かす。
    return -(std::cos(std::numbers::pi_v<float> * t) - 1.0f) / 2.0f;
}

float Easing::InQuad(float t) {
    // 二次曲線で開始直後を遅くし、一定の加速感を作る。
    return t * t;
}

float Easing::OutQuad(float t) {
    // 二次曲線を反転させ、終点へ向けて減速する。
    return 1.0f - (1.0f - t) * (1.0f - t);
}

float Easing::InOutQuad(float t) {
    // 前半を二次加速、後半を対称な減速としてつなぐ。
    return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) / 2.0f;
}

float Easing::InCubic(float t) {
    // 二次より強く始点を抑え、後半の加速を目立たせる。
    return t * t * t;
}

float Easing::OutCubic(float t) {
    // 三次曲線を反転させ、終点直前まで減速感を残す。
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

float Easing::InOutCubic(float t) {
    // 中点で加速と減速を切り替える三次の対称補間。
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

float Easing::InQuart(float t) {
    // 始動をより長く抑え、終盤で大きく速度を上げる。
    return t * t * t * t;
}

float Easing::OutQuart(float t) {
    // 終端で強く速度を落とす四次曲線。
    return 1.0f - std::pow(1.0f - t, 4.0f);
}

float Easing::InOutQuart(float t) {
    // 四次の加速と減速を半分ずつ使い、両端をゆっくり動かす。
    return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 4.0f) / 2.0f;
}

float Easing::InQuint(float t) {
    // 五次曲線で始点付近を強く抑え、急な終盤加速を作る。
    return t * t * t * t * t;
}

float Easing::OutQuint(float t) {
    // 五次曲線を反転させ、開始側に速度を寄せる。
    return 1.0f - std::pow(1.0f - t, 5.0f);
}

float Easing::InOutQuint(float t) {
    // 五次の加減速を中点でつなぎ、両端の動きをより強く抑える。
    return t < 0.5f ? 16.0f * t * t * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 5.0f) / 2.0f;
}

float Easing::InExpo(float t) {
    // 指数的な加速を使うが、始点は厳密に0へ固定する。
    return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * t - 10.0f);
}

float Easing::OutExpo(float t) {
    // 指数的な減速を使い、終点は厳密に1へ固定する。
    return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
}

float Easing::InOutExpo(float t) {
    // 始点・終点を厳密に固定し、前半加速と後半減速を指数曲線でつなぐ。
    // 始点では指数式の近似値を使わず、厳密に0を返す。
    if (t == 0.0f)
        return 0.0f;
    // 終端でも指数式を使わず、厳密に1を返す。
    if (t == 1.0f)
        return 1.0f;
    // 前半・後半の各半区間を長さ1に換算し、出力の変化幅は半分にして中点0.5へつなぎます。
    // 2倍した進行率が1未満なら前半の加速を使い、中央から後半は減速を使う。
    if ((t *= 2.0f) < 1.0f)
        return std::pow(2.0f, 10.0f * (t - 1.0f)) / 2.0f;
    return (2.0f - std::pow(2.0f, -10.0f * (t - 1.0f))) / 2.0f;
}

float Easing::InCirc(float t) {
    // 円弧の形で始点を抑え、終点へ向けて急に加速する。
    return 1.0f - std::sqrt(1.0f - std::pow(t, 2.0f));
}

float Easing::OutCirc(float t) {
    // 円弧を反転させ、開始時に速く終点付近でゆっくり動かす。
    return std::sqrt(1.0f - std::pow(t - 1.0f, 2.0f));
}

float Easing::InOutCirc(float t) {
    // 円弧の加速と減速を中点で対称につなぐ。
    return t < 0.5f
        ? (1.0f - std::sqrt(1.0f - std::pow(2.0f * t, 2.0f))) / 2.0f
        : (std::sqrt(1.0f - std::pow(-2.0f * t + 2.0f, 2.0f)) + 1.0f) / 2.0f;
}

float Easing::InBack(float t) {
    // 正方向へ進む前に少し逆へ引き、予備動作を見せる。
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}

float Easing::OutBack(float t) {
    // 終点を少し通り過ぎて戻り、停止時の反動を表現する。
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

float Easing::InOutBack(float t) {
    // 始点の引きと終点の行き過ぎを両方入れる。
    const float c1 = 1.70158f;
    const float c2 = c1 * 1.525f;
    return t < 0.5f
        ? (std::pow(2.0f * t, 2.0f) * ((c2 + 1.0f) * 2.0f * t - c2)) / 2.0f
        : (std::pow(2.0f * t - 2.0f, 2.0f) * ((c2 + 1.0f) * (2.0f * t - 2.0f) + c2) + 2.0f) / 2.0f;
}

float Easing::InElastic(float t) {
    // 開始側で振動しながら振幅を増やし、ばねの解放前を表現する。
    const float c4 = (2.0f * std::numbers::pi_v<float>) / 3.0f;
    // 端点では振動式を通さず、補間の始点と終点を正確に返す。
    if (t == 0.0f) {
        return 0.0f;
    }
    if (t == 1.0f) {
        return 1.0f;
    }
    return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * c4);
}

float Easing::OutElastic(float t) {
    // 終点をまたぐ振動を減衰させ、ばねが収まる動きを作る。
    const float c4 = (2.0f * std::numbers::pi_v<float>) / 3.0f;
    // 端点では振動式を通さず、補間の始点と終点を正確に返す。
    if (t == 0.0f) {
        return 0.0f;
    }
    if (t == 1.0f) {
        return 1.0f;
    }
    return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * c4) + 1.0f;
}

float Easing::InOutElastic(float t) {
    // 前半で振幅を増やし後半で減衰させ、両端を正確な値へ戻す。
    const float c5 = (2.0f * std::numbers::pi_v<float>) / 4.5f;
    // 端点では振動式を通さず、補間の始点と終点を正確に返す。
    if (t == 0.0f) {
        return 0.0f;
    }
    if (t == 1.0f) {
        return 1.0f;
    }
    if (t < 0.5f) {
        return -(std::pow(2.0f, 20.0f * t - 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f;
    }
    return (std::pow(2.0f, -20.0f * t + 10.0f) * std::sin((20.0f * t - 11.125f) * c5)) / 2.0f + 1.0f;
}

float Easing::OutBounce(float t) {
    // 衝突ごとに跳ね返り幅が小さくなる区分二次曲線を使う。
    const float n1 = 7.5625f;
    const float d1 = 2.75f;

    // 最初の衝突までの区間は、進行率1へ向かう落下を表す。
    if (t < 1.0f / d1) {
        // 後続の跳ね返りより前の区間なので、時間や高さの補正を加えない。
        return n1 * t * t;
    // 最初の衝突を過ぎ、2/d1未満なら最も大きな跳ね返りを描く。
    } else if (t < 2.0f / d1) {
        return n1 * (t -= 1.5f / d1) * t + 0.75f;
    // 前の跳ね返りの後で2.5/d1未満なら、終端へ向けて跳ね返り幅をさらに小さくする。
    } else if (t < 2.5f / d1) {
        return n1 * (t -= 2.25f / d1) * t + 0.9375f;
    // 残りの区間は最も小さい跳ね返りで終端へ近づける。
    } else {
        return n1 * (t -= 2.625f / d1) * t + 0.984375f;
    }
}

float Easing::InBounce(float t) {
    // 終点で跳ねる曲線を時間反転し、始点側に跳ねを移す。
    return 1.0f - OutBounce(1.0f - t);
}

float Easing::InOutBounce(float t) {
    // 時間反転した跳ねと通常の跳ねを半分ずつつなぐ。
    return t < 0.5f
        ? (1.0f - OutBounce(1.0f - 2.0f * t)) / 2.0f
        : (1.0f + OutBounce(2.0f * t - 1.0f)) / 2.0f;
}

// 0〜1の両端で変化率が0になる三次曲線。
float Easing::SmoothStep(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// 0〜1の範囲外では端点に収めてから、両端の勾配が0の補間率を求める。
float Easing::SmoothStep01(float t) {
    return SmoothStep(std::clamp(t, 0.0f, 1.0f));
}

float Easing::OutQuad01(float t) {
    // 経過時間が演出区間の前後でも、端点の姿勢を保つ。
    return OutQuad(std::clamp(t, 0.0f, 1.0f));
}

float Easing::OutCubic01(float t) {
    // 経過時間が演出区間の前後でも、端点の姿勢を保つ。
    return OutCubic(std::clamp(t, 0.0f, 1.0f));
}

float Easing::InOutCubic01(float t) {
    // 経過時間が演出区間の前後でも、端点の姿勢を保つ。
    return InOutCubic(std::clamp(t, 0.0f, 1.0f));
}

float Easing::OutBack01(float t) {
    // 入力だけを範囲に収め、終点手前で一度1を超える曲線はそのまま使う。
    return OutBack(std::clamp(t, 0.0f, 1.0f));
}

float Easing::OutQuint01(float t) {
    // 経過時間が演出区間の前後でも、端点の姿勢を保つ。
    return OutQuint(std::clamp(t, 0.0f, 1.0f));
}

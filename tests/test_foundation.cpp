#include <QTest>
#include <concepts>
#include <numbers>
#include <cmath>

class TestFoundation : public QObject
{
    Q_OBJECT

private slots:
    void testCpp20Features();
    void testFloat64Precision();
};

template <typename T>
concept NumericFloat = std::is_floating_point_v<T>;

void TestFoundation::testCpp20Features()
{
    // Verify C++20 concepts and standard library constants
    static_assert(NumericFloat<double>);
    static_assert(sizeof(double) == 8);

    double pi = std::numbers::pi_v<double>;
    QVERIFY(std::abs(pi - 3.14159265358979323846) < 1e-15);
}

void TestFoundation::testFloat64Precision()
{
    // Test IEEE 754 float64 dynamic range & headroom (critical for internal Audio Engine)
    double baseline = 1.0;
    double attenuated = baseline * std::pow(10.0, -144.0 / 20.0); // -144 dB (24-bit dynamic range floor)
    QVERIFY(attenuated > 0.0);

    // Summing test (comparing float32 vs float64 accumulation error)
    // 100,000 accumulation iterations of 1.0 / 100,000
    const int iterations = 100000;
    const double delta64 = 1.0 / iterations;
    const float delta32 = 1.0f / static_cast<float>(iterations);

    double sum64 = 0.0;
    float sum32 = 0.0f;
    for (int i = 0; i < iterations; ++i) {
        sum64 += delta64;
        sum32 += delta32;
    }

    double err64 = std::abs(sum64 - 1.0);
    double err32 = std::abs(static_cast<double>(sum32) - 1.0);

    // float64 accumulation error is orders of magnitude smaller than float32
    QVERIFY(err64 < 1e-9);
    QVERIFY(err32 > 1e-4);
    QVERIFY(err64 < err32);
}

QTEST_MAIN(TestFoundation)
#include "test_foundation.moc"

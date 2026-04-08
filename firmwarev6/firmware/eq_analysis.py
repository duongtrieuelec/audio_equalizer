import numpy as np
from scipy import signal
import matplotlib.pyplot as plt
from scipy.signal import savgol_filter

class Filter:
    def __init__(self):
        self.a0 = self.a1 = self.a2 = self.b1 = self.b2 = 0.0

def get_filter_coefficients(filter_type, freq, sample_rate, gain_db, Q=0.707):
    """Tính hệ số bộ lọc"""
    gain_db = min(max(gain_db, -40), 6)
    V = np.power(10, abs(gain_db) / 20.0)
    K = np.tan(np.pi * freq / sample_rate)
    
    filter = Filter()
    
    if filter_type == "lowshelf":
        if gain_db >= 0:
            norm = 1 / (1 + np.sqrt(2) * K + K * K)
            filter.a0 = (1 + np.sqrt(2*V) * K + V * K * K) * norm
            filter.a1 = 2 * (V * K * K - 1) * norm
            filter.a2 = (1 - np.sqrt(2*V) * K + V * K * K) * norm
            filter.b1 = 2 * (K * K - 1) * norm
            filter.b2 = (1 - np.sqrt(2) * K + K * K) * norm
        else:
            norm = 1 / (1 + np.sqrt(2*V) * K + V * K * K)
            filter.a0 = (1 + np.sqrt(2) * K + K * K) * norm
            filter.a1 = 2 * (K * K - 1) * norm
            filter.a2 = (1 - np.sqrt(2) * K + K * K) * norm
            filter.b1 = 2 * (V * K * K - 1) * norm
            filter.b2 = (1 - np.sqrt(2*V) * K + V * K * K) * norm
            
    elif filter_type == "peak":
        Q = 2.5
        if gain_db >= 0:
            norm = 1 / (1 + 1/Q * K + K * K)
            filter.a0 = (1 + V/Q * K + K * K) * norm
            filter.a1 = 2 * (K * K - 1) * norm
            filter.a2 = (1 - V/Q * K + K * K) * norm
            filter.b1 = filter.a1
            filter.b2 = (1 - 1/Q * K + K * K) * norm
        else:
            norm = 1 / (1 + V/Q * K + K * K)
            filter.a0 = (1 + 1/Q * K + K * K) * norm
            filter.a1 = 2 * (K * K - 1) * norm
            filter.a2 = (1 - 1/Q * K + K * K) * norm
            filter.b1 = filter.a1
            filter.b2 = (1 - V/Q * K + K * K) * norm
            
    elif filter_type == "highshelf":
        if gain_db >= 0:
            norm = 1 / (1 + np.sqrt(2) * K + K * K)
            filter.a0 = (V + np.sqrt(2*V) * K + K * K) * norm
            filter.a1 = 2 * (K * K - V) * norm
            filter.a2 = (V - np.sqrt(2*V) * K + K * K) * norm
            filter.b1 = 2 * (K * K - 1) * norm
            filter.b2 = (1 - np.sqrt(2) * K + K * K) * norm
        else:
            norm = 1 / (V + np.sqrt(2*V) * K + K * K)
            filter.a0 = (1 + np.sqrt(2) * K + K * K) * norm
            filter.a1 = 2 * (K * K - 1) * norm
            filter.a2 = (1 - np.sqrt(2) * K + K * K) * norm
            filter.b1 = 2 * (K * K - V) * norm
            filter.b2 = (V - np.sqrt(2*V) * K + K * K) * norm
            
    return [filter.a0, filter.a1, filter.a2], [1.0, filter.b1, filter.b2]

def compute_frequency_response(sample_rate=44100, num_points=1000):
    """Tính các điểm tần số để phân tích"""
    return np.logspace(np.log10(20), np.log10(20000), num_points)

def analyze_filter_response(frequencies, sample_rate, low_gain, mid_gain, high_gain):
    """Phân tích đáp ứng của bộ lọc"""
    # Tính hệ số cho từng bộ lọc
    a_low, b_low = get_filter_coefficients("lowshelf", 250, sample_rate, low_gain)
    a_mid, b_mid = get_filter_coefficients("peak", 1500, sample_rate, mid_gain)
    a_high, b_high = get_filter_coefficients("highshelf", 4500, sample_rate, high_gain)

    # Tính w cho freqz
    w = 2 * np.pi * frequencies / sample_rate

    # Tính đáp ứng của từng bộ lọc
    _, h_low = signal.freqz(a_low, b_low, worN=w)
    _, h_mid = signal.freqz(a_mid, b_mid, worN=w)
    _, h_high = signal.freqz(a_high, b_high, worN=w)

    # Tổng hợp đáp ứng
    h_total = h_low * h_mid * h_high

    # Tính magnitude trong dB
    magnitude_db = 20 * np.log10(np.abs(h_total))

    # Tính phase
    phase = np.unwrap(np.angle(h_total))

    # Tính group delay
    group_delay = -np.diff(phase) / (2 * np.pi * np.diff(frequencies))  # seconds
    group_delay = group_delay * 1000  # Convert to milliseconds

    # Smooth group delay
    window_length = min(51, len(group_delay) - 1)
    if window_length % 2 == 0:
        window_length -= 1
    if window_length >= 3:
        group_delay = savgol_filter(group_delay, window_length, 3)

    return magnitude_db, phase, group_delay

def plot_eq_analysis(sample_rate=44100):
    """Vẽ đồ thị phân tích EQ"""
    # Ba preset cơ bản
    eq_presets = {
        'Rock': {'low': 8, 'mid': -4, 'high': 8},
        'Pop': {'low': 5, 'mid': 5, 'high': 5},
        'Jazz': {'low': -2, 'mid': 5, 'high': 5}
    }

    frequencies = compute_frequency_response(sample_rate)

    # Tạo figure với 3 subplot
    plt.style.use('seaborn-darkgrid')  # Sử dụng style đẹp hơn
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 12))
    plt.subplots_adjust(top=0.92)
    fig.suptitle('3-Band EQ Analysis', size=14, weight='bold')

    # Plot cho mỗi preset
    colors = {'Rock': '#FF4B4B', 'Pop': '#4B4BFF', 'Jazz': '#4BFF4B'}
    line_styles = {'Rock': '-', 'Pop': '--', 'Jazz': '-.'}

    for preset_name, gains in eq_presets.items():
        magnitude, phase, group_delay = analyze_filter_response(
            frequencies, sample_rate,
            gains['low'], gains['mid'], gains['high']
        )

        style = line_styles[preset_name]
        color = colors[preset_name]

        # Magnitude response
        ax1.semilogx(frequencies, magnitude,
                    label=f"{preset_name} (L:{gains['low']}dB M:{gains['mid']}dB H:{gains['high']}dB)",
                    color=color, linestyle=style)

        # Phase response
        ax2.semilogx(frequencies, np.rad2deg(phase), 
                    label=preset_name, color=color, linestyle=style)

        # Group delay
        ax3.semilogx(frequencies[:-1], group_delay, 
                    label=preset_name, color=color, linestyle=style)

    # Cấu hình chung cho tất cả các trục
    for ax in [ax1, ax2, ax3]:
        ax.grid(True, which='both', alpha=0.3)
        ax.minorticks_on()
        ax.set_xlim(20, 20000)
        # Đặt các điểm chia chính trên trục x
        ax.set_xticks([20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000])
        ax.set_xticklabels(['20', '50', '100', '200', '500', '1k', '2k', '5k', '10k', '20k'])
        ax.legend(loc='upper right')

    # Cấu hình riêng cho từng plot
    ax1.set_ylabel('Magnitude [dB]', weight='bold')
    ax1.set_title('Magnitude Response', pad=10, weight='bold')
    ax1.set_ylim(-15, 20)

    ax2.set_ylabel('Phase [degrees]', weight='bold')
    ax2.set_title('Phase Response', pad=10, weight='bold')

    ax3.set_ylabel('Group Delay [ms]', weight='bold')
    ax3.set_xlabel('Frequency [Hz]', weight='bold')
    ax3.set_title('Group Delay', pad=10, weight='bold')
    ax3.set_ylim(-2, 2)

    # Thêm đường đánh dấu tần số cắt và tham chiếu
    cutoff_freqs = [250, 1500, 4500]
    cutoff_labels = ['250Hz', '1.5kHz', '4.5kHz']
    
    for ax in [ax1, ax2, ax3]:
        # Thêm đường dọc cho tần số cắt
        for freq, label in zip(cutoff_freqs, cutoff_labels):
            ax.axvline(freq, color='gray', linestyle=':', alpha=0.5)
            # Thêm nhãn cho tần số cắt
            ax.text(freq, ax.get_ylim()[1], label, 
                   rotation=90, va='bottom', ha='right',
                   color='gray', alpha=0.7, fontsize=8)
        # Thêm đường ngang tại 0
        ax.axhline(0, color='black', linestyle='-', alpha=0.1)

    # Thêm các đường tham chiếu cho group delay
    for delay in [-1.5, -1, -0.5, 0.5, 1, 1.5]:
        ax3.axhline(delay, color='gray', linestyle=':', alpha=0.2)

    plt.tight_layout()
    plt.show()

if __name__ == "__main__":
    plot_eq_analysis() 

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from scipy.fft import fft, fftfreq
from scipy.signal import butter, filtfilt, find_peaks

# ==========================================
# 1. FUNCIÓN PARA LEER DATOS Y CONFIGURACIÓN
# ==========================================
fs = 100.0  # Frecuencia de muestreo (Hz)
segundos_a_descartar = 5.0
archivo_csv = '/content/pulgar_V4.csv'

# Leer el CSV
df = pd.read_csv(archivo_csv)

# Tomamos las columnas de los LEDs por su nombre
red = df['red'].values
ir = df['ir'].values
red_raw = red[1000:6000]
ir_raw = ir[1000:6000]


# Reconstruimos un vector de tiempo en segundos para el eje X de los gráficos
tiempo = np.arange(len(red_raw)) / fs

# ==========================================
# 2. DISEÑO DE FILTROS (Butterworth)
# ==========================================
def bandpass_filter(data, lowcut, highcut, fs, order=4):
    nyq = 0.5 * fs
    low = lowcut / nyq
    high = highcut / nyq
    b, a = butter(order, [low, high], btype='band')
    return filtfilt(b, a, data)

def lowpass_filter(data, cutoff, fs, order=4):
    nyq = 0.5 * fs
    normal_cutoff = cutoff / nyq
    b, a = butter(order, normal_cutoff, btype='low', analog=False)
    return filtfilt(b, a, data)

def highpass_filter(data, cutoff, fs, order=4):
    nyq = 0.5 * fs
    normal_cutoff = cutoff / nyq
    b, a = butter(order, normal_cutoff, btype='high', analog=False)
    return filtfilt(b, a, data)

# ==========================================
# 3. PROCESAMIENTO DE LA SEÑAL Y SpO2
# ==========================================
# Extraer Componente AC (Filtro pasabanda 0.5 Hz - 4 Hz) e invertir la señal (* -1)
red_ac = -1.0 * bandpass_filter(red_raw, 0.5, 4.0, fs)
ir_ac = -1.0 * bandpass_filter(ir_raw, 0.5, 4.0, fs)

# Extraer Componente DC (Filtro pasabajos < 0.2 Hz)
red_dc = lowpass_filter(red_raw, 0.2, fs)
ir_dc = lowpass_filter(ir_raw, 0.2, fs)

# Calculamos el valor RMS de las señales AC
red_ac_rms = np.sqrt(np.mean(red_ac**2))
ir_ac_rms = np.sqrt(np.mean(ir_ac**2))

# Calculamos el nivel DC medio
red_dc_mean = np.mean(red_dc)
ir_dc_mean = np.mean(ir_dc)

# Fórmula de Ratio (R)
R = (red_ac_rms / red_dc_mean) / (ir_ac_rms / ir_dc_mean)

# Ecuación cuadrática empírica oficial para el MAX30102
SpO2 = -45.060 * (R**2) + 30.354 * R + 94.845

# Límite de seguridad: el modelo matemático puede dar > 100 si R es atípico
if SpO2 > 100:
    SpO2 = 100.0

print(f"\n--- RESULTADOS ---")
print(f"Valor de R calculado: {R:.3f}")
print(f"SpO2 Estimado (MAX30102): {SpO2:.1f} %")

# ==========================================
# 4. ANÁLISIS ESPECTRAL 1 (Restando Media)
# ==========================================
# Filtramos para quedarnos con los datos a partir del segundo 5
mascara_tiempo = tiempo >= segundos_a_descartar
tiempo_fft = tiempo[mascara_tiempo]
señal_analisis_1 = ir_raw[mascara_tiempo] - np.mean(ir_raw[mascara_tiempo])

# Cálculo de la FFT
N1 = len(señal_analisis_1)
yf1 = np.abs(fft(señal_analisis_1))[:N1//2]
xf1 = fftfreq(N1, 1 / fs)[:N1//2]

# --- EXTRACTOR DE FRECUENCIA RESPIRATORIA ---
banda_resp_1 = (xf1 >= 0.1) & (xf1 <= 0.5)
frec_resp_hz_1 = xf1[banda_resp_1][np.argmax(yf1[banda_resp_1])]
respiraciones_por_minuto_1 = frec_resp_hz_1 * 60

# --- EXTRACTOR DE FRECUENCIA CARDÍACA ---
banda_card_1 = (xf1 >= 0.8) & (xf1 <= 3.0)
frec_card_hz_1 = xf1[banda_card_1][np.argmax(yf1[banda_card_1])]
latidos_por_minuto_1 = frec_card_hz_1 * 60

#print(f"\n--- ANÁLISIS ESPECTRAL (Desde t = 5s) ---")
print(f"Frecuencia Cardíaca: {latidos_por_minuto_1:.1f} BPM ({frec_card_hz_1:.3f} Hz)")

# ==========================================
# 5. ANÁLISIS ESPECTRAL 2 (Filtrado HPF)
# ==========================================
ir_filtrada = highpass_filter(ir_raw, 0.5, fs, order=4)
señal_analisis_2 = ir_filtrada[mascara_tiempo] - np.mean(ir_filtrada[mascara_tiempo])

# Cálculo de la FFT
N2 = len(señal_analisis_2)
yf2 = np.abs(fft(señal_analisis_2))[:N2//2]
xf2 = fftfreq(N2, 1 / fs)[:N2//2]

# Banda respiratoria
banda_resp_2 = (xf2 >= 0.1) & (xf2 <= 0.5)
frec_resp_hz_2 = xf2[banda_resp_2][np.argmax(yf2[banda_resp_2])]
respiraciones_por_minuto_2 = frec_resp_hz_2 * 60

# Banda cardíaca
banda_card_2 = (xf2 >= 0.8) & (xf2 <= 3.0)
frec_card_hz_2 = xf2[banda_card_2][np.argmax(yf2[banda_card_2])]
latidos_por_minuto_2 = frec_card_hz_2 * 60

# ==========================================
# 6. VISUALIZACIÓN DE SEÑAL
# =======================
ventana_muestras = int(5 * fs)
plt.plot(tiempo[:ventana_muestras], ir_ac[:ventana_muestras], color='black', label='Onda AC IR')
plt.title('Onda de Pulso Filtrada (Filtro Pasabanda 0.5 - 4 Hz) - Ventana de 5s')  
plt.xlabel('Tiempo (segundos)')
plt.ylabel('Amplitud AC')
plt.legend()
plt.grid(True)

plt.tight_layout()
plt.show()
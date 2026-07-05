from datetime import datetime
import sqlite3
import os
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel

app = FastAPI()

# Caminho da base de dados persistente
BASE_DIR = Path(__file__).resolve().parent
DB_FILE = BASE_DIR / "data" / "iot_config.db"
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))

# --- Modelos de Dados ---
class MarcaConfig(BaseModel):
    marca: str
    temp_max: float
    temp_min: float
    temp_alvo: int
    modo_teste: bool = False # <-- Adicionado para receber o estado do UI

class EstadoPayload(BaseModel):
    marca: str

class TelemetriaPayload(BaseModel):
    temperatura: float
    ar_ligado: bool
    acao: str
    log: str

# --- Inicialização da Base de Dados ---
def init_db():
    os.makedirs(os.path.dirname(DB_FILE), exist_ok=True)
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    # 1. Catálogo de Configurações por marca
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS config (
            marca TEXT PRIMARY KEY,
            temp_max REAL,
            temp_min REAL,
            temp_alvo INTEGER,
            modo_teste BOOLEAN DEFAULT 0
        )
    ''')
    
    # 2. Estado Atual (Ponteiro da marca ativa)
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS estado_atual (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            marca_ativa TEXT
        )
    ''')
    
    # 3. Telemetria e Logs
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS telemetria (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            temperatura REAL,
            ar_ligado BOOLEAN,
            acao TEXT,
            log TEXT,
            timestamp DATETIME DEFAULT (datetime('now', 'localtime'))
        )
    ''')
    
    # Valores por defeito (Com o 0 no final para o modo_teste)
    cursor.execute('INSERT OR IGNORE INTO config VALUES ("LG", 26.0, 22.0, 20, 0)')
    cursor.execute('INSERT OR IGNORE INTO estado_atual VALUES (1, "LG")')
    conn.commit()
    conn.close()

init_db()

# --- Rotas da Interface ---
@app.get("/", response_class=HTMLResponse)
async def renderizar_dashboard(request: Request):
    return templates.TemplateResponse(
        request=request, 
        name="dashboard.html"
    )

# --- Rotas da API ---
@app.get("/api/getConfig")
def get_config():
    """O ESP32 consulta: qual a configuração da marca ativa?"""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    # Obtém a marca ativa
    cursor.execute("SELECT marca_ativa FROM estado_atual WHERE id = 1")
    marca_ativa = cursor.fetchone()[0]
    
    # Busca dados no catálogo (agora inclui o modo_teste)
    cursor.execute("SELECT temp_max, temp_min, temp_alvo, modo_teste FROM config WHERE marca = ?", (marca_ativa,))
    row = cursor.fetchone()
    conn.close()

    if row:
        print(f"[API GET_CONFIG] ESP32 consultou as regras. Marca ativa: {marca_ativa} | Teste: {bool(row[3])}")
        return {
            "marca": marca_ativa, 
            "alerta": row[0], 
            "desligar": row[1], 
            "alvo": row[2], 
            "modo_teste": bool(row[3])
        }
    
    print("[API GET_CONFIG] ERRO: Configuração da marca ativa não encontrada.")
    return {"erro": "Configuração da marca ativa não encontrada"}

@app.post("/api/config")
def upsert_config(dados: MarcaConfig):
    """Adiciona/Edita marcas no catálogo e altera a marca ativa."""
    print(f"\n[API UI] Recebida nova configuração pelo Dashboard! Marca: {dados.marca} | Max: {dados.temp_max} | Min: {dados.temp_min} | Teste: {dados.modo_teste}")
    
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO config (marca, temp_max, temp_min, temp_alvo, modo_teste)
        VALUES (?, ?, ?, ?, ?)
        ON CONFLICT(marca) DO UPDATE SET
            temp_max=excluded.temp_max,
            temp_min=excluded.temp_min,
            temp_alvo=excluded.temp_alvo,
            modo_teste=excluded.modo_teste
    ''', (dados.marca, dados.temp_max, dados.temp_min, dados.temp_alvo, dados.modo_teste))
    
    # Garante que ao guardar na UI, esta passa a ser a marca ativa imediatamente
    cursor.execute("UPDATE estado_atual SET marca_ativa = ? WHERE id = 1", (dados.marca,))
    
    conn.commit()
    conn.close()
    return {"mensagem": f"Marca {dados.marca} guardada no catálogo e ativada."}

@app.post("/api/estado")
def set_estado(dados: EstadoPayload):
    """O Dashboard define qual a marca que o ESP32 deve usar."""
    print(f"[API UI] Estado alterado manualmente para a marca: {dados.marca}")
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute("UPDATE estado_atual SET marca_ativa = ? WHERE id = 1", (dados.marca,))
    conn.commit()
    conn.close()
    return {"mensagem": f"Estado alterado para {dados.marca}"}

# --- ROTAS DE TELEMETRIA E GRÁFICO ---

@app.post("/api/telemetria")
def receber_telemetria(dados: TelemetriaPayload):
    print(f"\n[ESP32 -> API] Telemetria: Temp={dados.temperatura}°C | AC={'Ligado' if dados.ar_ligado else 'Desligado'} | Ação={dados.acao} | Log={dados.log}")
    
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    # Inserção na tabela correta (telemetria)
    cursor.execute('''
        INSERT INTO telemetria (temperatura, ar_ligado, acao, log, timestamp) 
        VALUES (?, ?, ?, ?, ?)
    ''', (dados.temperatura, dados.ar_ligado, dados.acao, dados.log, datetime.now().strftime("%H:%M:%S")))
    
    conn.commit()
    conn.close()
    return {"status": "ok"}

@app.get("/api/logs")
def get_logs():
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    # Consulta na tabela correta (telemetria)
    cursor.execute("SELECT timestamp, temperatura, ar_ligado, log FROM telemetria ORDER BY id DESC LIMIT 20")
    
    logs = []
    for row in cursor.fetchall():
        timestamp, temp, ac_on, msg = row
        status_ac = "LIGADO" if ac_on else "DESLIGADO"
        texto_log = f"Temp: {temp}°C | AC: {status_ac} | {msg}"
        logs.append({"timestamp": timestamp, "log": texto_log})
        
    conn.close()
    return logs

@app.get("/api/historico")
def get_historico():
    """ Rota para o gráfico funcionar! """
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    # Aumentado para LIMIT 120 para ter melhor histórico no gráfico (2 horas se enviar a cada 1 min)
    cursor.execute("SELECT timestamp, temperatura FROM telemetria ORDER BY id DESC LIMIT 120")
    rows = cursor.fetchall()
    conn.close()
    
    rows.reverse() # Inverte para mostrar da esquerda (antigo) para a direita (novo) no gráfico
    return {
        "labels": [row[0] for row in rows],
        "temperaturas": [row[1] for row in rows]
    }
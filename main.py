from datetime import datetime
import sqlite3
import os
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel

app = FastAPI()

# Caminho do banco de dados persistente
BASE_DIR = Path(__file__).resolve().parent
DB_FILE = BASE_DIR / "data" / "iot_config.db"
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))

# --- Modelos de Dados ---
class MarcaConfig(BaseModel):
    marca: str
    temp_max: int
    temp_min: int
    temp_alvo: int

class EstadoPayload(BaseModel):
    marca: str

class TelemetriaPayload(BaseModel):
    temperatura: float
    ar_ligado: bool
    acao: str
    log: str

# --- Inicialização do Banco ---
def init_db():
    os.makedirs(os.path.dirname(DB_FILE), exist_ok=True)
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    # 1. Catálogo de Configurações por marca
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS config (
            marca TEXT PRIMARY KEY,
            temp_max INTEGER,
            temp_min INTEGER,
            temp_alvo INTEGER
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
    
    # Defaults
    cursor.execute('INSERT OR IGNORE INTO config VALUES ("LG", 26, 22, 20)')
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
    """ESP32 consulta: qual a configuração da marca ativa?"""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    
    # Pega marca ativa
    cursor.execute("SELECT marca_ativa FROM estado_atual WHERE id = 1")
    marca_ativa = cursor.fetchone()[0]
    
    # Busca dados no catálogo
    cursor.execute("SELECT temp_max, temp_min, temp_alvo FROM config WHERE marca = ?", (marca_ativa,))
    row = cursor.fetchone()
    conn.close()

    if row:
        return {"marca": marca_ativa, "alerta": row[0], "desligar": row[1], "alvo": row[2]}
    return {"erro": "Configuração da marca ativa não encontrada"}

@app.post("/api/config")
def upsert_config(dados: MarcaConfig):
    """Adiciona/Edita marcas no catálogo."""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO config (marca, temp_max, temp_min, temp_alvo)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(marca) DO UPDATE SET
            temp_max=excluded.temp_max,
            temp_min=excluded.temp_min,
            temp_alvo=excluded.temp_alvo
    ''', (dados.marca, dados.temp_max, dados.temp_min, dados.temp_alvo))
    conn.commit()
    conn.close()
    return {"mensagem": f"Marca {dados.marca} salva no catálogo."}

@app.post("/api/estado")
def set_estado(dados: EstadoPayload):
    """Dashboard define qual marca o ESP32 deve usar."""
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute("UPDATE estado_atual SET marca_ativa = ? WHERE id = 1", (dados.marca,))
    conn.commit()
    conn.close()
    return {"mensagem": f"Estado alterado para {dados.marca}"}

# --- ROTAS DE TESTE PARA LOGS E GRÁFICO ---

@app.post("/api/telemetria")
def receber_telemetria(dados: Telemetria):
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    msg = f"Temp: {dados.temperatura}°C - {dados.log}"
    cursor.execute("INSERT INTO logs (timestamp, log) VALUES (?, ?)", (datetime.now().strftime("%H:%M:%S"), msg))
    conn.commit()
    conn.close()
    return {"status": "ok"}

@app.get("/api/logs")
def get_logs():
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute("SELECT timestamp, log FROM logs ORDER BY id DESC LIMIT 20")
    logs = [{"timestamp": row[0], "log": row[1]} for row in cursor.fetchall()]
    conn.close()
    return logs
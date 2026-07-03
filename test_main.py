import pytest
from fastapi.testclient import TestClient
from main import app 

client = TestClient(app)

@pytest.fixture
def payload_config():
    # Estrutura esperada pelo seu main.py atual (conforme dashboard.html)
    return {
        "marca": "LG",
        "temp_max": 26,
        "temp_min": 22,
        "temp_alvo": 20
    }

def test_deve_salvar_configuracao_com_sucesso(payload_config):
    # Rota correta conforme seu main.py: /api/config (POST)
    response = client.post("/api/config", json=payload_config)
    
    assert response.status_code == 200
    assert "marca lg salva no catálogo." in response.json()["mensagem"].lower()

def test_deve_recuperar_configuracao_correta():
    # Rota correta conforme seu main.py: /api/getConfig (GET)
    response = client.get("/api/getConfig")
    
    assert response.status_code == 200
    data = response.json()
    # Verifica se os dados batem com o que foi configurado
    assert data["marca"] == "LG"
    assert data["alerta"] == 26

def test_validacao_de_schema_retorna_422_com_dados_invalidos():
    payload_invalido = {"marca_errada": 123}
    # O FastAPI deve retornar 422 se o schema não for validado
    response = client.post("/api/config", json=payload_invalido)
    assert response.status_code == 422
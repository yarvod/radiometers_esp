def test_create_app_import():
    from app.main import create_app

    app = create_app()
    assert app.title == "Radiometer API"

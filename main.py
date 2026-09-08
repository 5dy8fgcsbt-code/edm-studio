if __name__=='__main__':
    import multiprocessing
    multiprocessing.freeze_support()
    from edm_studio.app import main
    raise SystemExit(main())

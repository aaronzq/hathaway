import queue
import time
import unittest

import control_panel


class DummyDB:
    def start_session(self, rig_id, note):
        return 1

    def write(self, samples, events):
        pass

    def close(self):
        pass


class DrainingLink:
    def __init__(self, ctrl, port):
        self.ctrl = ctrl
        self.port = port
        self.stopped = False
        self.joined = False

    def stop(self):
        self.stopped = True

    def join(self, timeout=None):
        self.ctrl.on_line(self.port, "1|LICK:1,1,123")
        self.joined = True


class ClosePortTest(unittest.TestCase):
    def test_close_port_drains_reader_before_ending_session(self):
        ctrl = control_panel.Controller(DummyDB())
        link = DrainingLink(ctrl, "COM1")
        ctrl.link_by_port["COM1"] = link
        ctrl.port_rig["COM1"] = 1

        ok, _ = ctrl.close_port("COM1")

        self.assertTrue(ok)
        items = []
        while True:
            try:
                items.append(ctrl.recq.get_nowait())
            except queue.Empty:
                break
        self.assertIsInstance(items[0][1], list)
        self.assertIs(items[1][1], ctrl.SESSION_END)

    def test_close_port_writes_drained_rows_before_closing_session(self):
        class RecordingDB:
            def __init__(self):
                self.calls = []

            def start_session(self, rig_id, note):
                self.calls.append(("start", rig_id))
                return 7

            def write(self, samples, events):
                self.calls.append(("write", len(samples), len(events)))

            def close_session(self, session_id):
                self.calls.append(("close_session", session_id))

            def close(self):
                self.calls.append(("close",))

        class ParamDrainingLink(DrainingLink):
            def join(self, timeout=None):
                self.ctrl.on_line(self.port, "1|PARAM:TEST,1,")
                self.joined = True

        db = RecordingDB()
        ctrl = control_panel.Controller(db)
        ctrl.start()
        link = ParamDrainingLink(ctrl, "COM1")
        ctrl.link_by_port["COM1"] = link
        ctrl.port_rig["COM1"] = 1

        ok, _ = ctrl.close_port("COM1")

        self.assertTrue(ok)
        for _ in range(20):
            if ("close_session", 7) in db.calls:
                break
            time.sleep(0.05)
        ctrl.shutdown()

        self.assertEqual(
            db.calls[:3],
            [("start", 1), ("write", 0, 1), ("close_session", 7)],
        )


if __name__ == "__main__":
    unittest.main()

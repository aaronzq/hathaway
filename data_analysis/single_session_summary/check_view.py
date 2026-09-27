"""Read-only raw-record inspection for the earlier task-3 review.
Run: python check_view.py. Requires psycopg2-binary and local PostgreSQL.
Uses events_dev directly; no trial view is required. Output: view_check.txt.
"""
from pathlib import Path
import psycopg2

queries = [
    ('Sessions', 'SELECT * FROM sessions WHERE session_id=ANY(%s) ORDER BY session_id', ([21,74],)),
    ('Recorded types and counter ranges', """SELECT session_id,type,count(*),
        min(value),max(value),count(*) FILTER (WHERE t_us=0) AS zero_device_times
        FROM events_dev WHERE session_id=ANY(%s) AND
        (type IN ('STATE','OUTCOME') OR starts_with(type,'PARAM_'))
        GROUP BY session_id,type ORDER BY session_id,type""", ([21,74],)),
    ('Counter decreases within each recorded event type', """WITH r AS (
        SELECT session_id,type,seq,value,lag(value) OVER
        (PARTITION BY session_id,rig_id,type ORDER BY seq) AS previous
        FROM events_dev WHERE session_id=ANY(%s) AND type IN ('STATE','OUTCOME'))
        SELECT * FROM r WHERE value<previous ORDER BY session_id,seq""", ([21,74],)),
]
output=[]
with psycopg2.connect('host=localhost port=5432 dbname=hathaway user=hathaway password=hathaway') as connection:
    connection.set_session(readonly=True)
    with connection.cursor() as cursor:
        for title,sql,params in queries:
            cursor.execute(sql,params)
            output += [title, ' | '.join(d.name for d in cursor.description)]
            output += [' | '.join(map(str,row)) for row in cursor.fetchall()]
            output += ['']
text='\n'.join(output)
Path(__file__).with_name('view_check.txt').write_text(text,encoding='utf-8')
print(text)

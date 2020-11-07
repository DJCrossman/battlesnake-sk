const bodyParser = require('body-parser')
const express = require('express')
const Main = require('./lib/main')

const PORT = process.env.PORT || 3000

const app = express()
let main = new Main()
app.use(bodyParser.json())

app.get('/', handleIndex)
app.post('/start', handleStart)
app.post('/move', handleMove)
app.post('/end', handleEnd)

app.listen(PORT, () => console.log(`Battlesnake Server listening at http://127.0.0.1:${PORT}`))


function handleIndex (request, response) {
  try {
    var battlesnakeInfo = {
      apiversion: '1',
      author: '',
      color: '#ff69b4',
      head: 'bendr',
      tail: 'freckled'
    }
    response.status(200).json(battlesnakeInfo)
  } catch (e) {
    console.error(e)
  }
}

async function handleStart (request, response) {
  try {
    console.log('START')
    main = new Main(request.body)
    response.status(200).send('ok')
  } catch (e) {
    console.error(e)
  }
}

async function handleMove (request, response) {
  try {
    const move = await main.handleMove(request.body)
    const shout = `I guess I'll go ${move} then.`
    console.log('MOVE: ' + shout)
    response.status(200).send({ move, shout })
  } catch (e) {
    console.error(e)
  }
}

async function handleEnd (request, response) {
  try {
    console.log('END')
    await main.handleEnd(request.body)
    response.status(200).send('ok')
  } catch (e) {
    console.error(e)
  }
}

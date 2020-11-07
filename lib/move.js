function moveAsCoord (move, head) {
  switch (move) {
    case 'up':
      return { x: head.x, y: head.y + 1 };
    case 'down':
      return { x: head.x, y: head.y - 1 };
    case 'left':
      return { x: head.x - 1, y: head.y };
    case 'right':
      return { x: head.x + 1, y: head.y };
  }
}

function offBoard (state, coord) {
  if (coord.x < 0) return true;
  if (coord.y < 0) return true;
  if (coord.y >= state.board.height) return true;
  if (coord.x >= state.board.height) return true;
  return false; // If it makes it here we are ok.
}

function onBody (body, coord) {
  return body.some((b) => coordEqual(b, coord))
}

function coordEqual (a, b) {
  return a.x === b.x && a.y === b.y;
}

module.exports = {
  moveAsCoord,
  offBoard,
  coordEqual,
  onBody
}